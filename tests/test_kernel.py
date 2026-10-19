#!/usr/bin/env python3
"""
make test：在无头 QEMU 中启动内核，检查启动输出、定时器、键盘和异常处理。

只用 Python 标准库。测试通过 QEMU 监视器（一个 unix socket）读取显存和寄存器、
模拟按键；用系统自带的 objdump 查符号地址和反汇编。
需要触发异常的测试，会用 make 的 SELFTEST 参数另外构建内核，放在 build/selftest/ 下。

在仓库根目录运行：python3 tests/test_kernel.py（make test 会先构建再调用它）。
"""
import datetime
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
QEMU = os.environ.get("QEMU", "qemu-system-i386")     # make test 会传入 Makefile 中的 QEMU
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


class TestFailure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise TestFailure(message)


class Machine:
    """一台无头 QEMU 虚拟机，用监视器命令观察和操作它。"""

    def __init__(self, image, snapshot=True):
        tmp = tempfile.mkdtemp(prefix="kernel-test-")
        self.sock_path = os.path.join(tmp, "monitor.sock")
        self.serial_path = os.path.join(tmp, "serial.sock")
        self.log_path = os.path.join(tmp, "qemu.log")
        self.sock = self.serial = None
        self.serial_buf, self.serial_pos = b"", 0
        # QEMU 的错误输出写进文件：用管道却不读，输出多了会把 QEMU 卡住
        # 同一目录下的 hd.img 接在主 IDE 通道上；snapshot=on 让写入只留在临时文件里，镜像本身不变。
        # snapshot=False 时写进镜像本身：只用在复制出来的镜像上，事后读它核对
        hd = os.path.join(os.path.dirname(image), "hd.img")
        disk = ["-drive", f"file={hd},format=raw,if=ide,index=0" + (",snapshot=on" if snapshot else "")] if os.path.exists(hd) else []
        # 第二块盘 hdb.img 接在从盘上，也一样
        hdb = os.path.join(os.path.dirname(image), "hdb.img")
        if os.path.exists(hdb):
            disk += ["-drive", f"file={hdb},format=raw,if=ide,index=1" + (",snapshot=on" if snapshot else "")]
        with open(self.log_path, "w") as log:
            self.proc = subprocess.Popen(
                [QEMU, "-machine", "pc", "-accel", "tcg", "-m", "16M",
                 "-drive", f"file={image},format=raw,if=floppy,index=0", *disk, "-boot", "order=a",
                 "-display", "none", "-nic", "none", "-serial", f"unix:{self.serial_path},server,nowait",
                 "-monitor", f"unix:{self.sock_path},server,nowait", "-no-reboot"],
                stdout=subprocess.DEVNULL, stderr=log)
        try:
            deadline = time.time() + 5
            while not os.path.exists(self.sock_path):
                check(self.proc.poll() is None, f"QEMU 启动失败：{self.log_tail()}")
                check(time.time() < deadline, "QEMU 没有创建监视器 socket")
                time.sleep(0.05)
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.connect(self.sock_path)
            self.sock.settimeout(0.2)
            self._read_until_prompt()
            # 串口接在另一个 socket 上：QEMU 是服务端，没人连的时候发出来的字符就丢了，所以开机前就连上。
            # 客户机打印的东西攒在 serial_buf 里，serial_wait() 从上次读到的地方往后找
            self.serial = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.serial.connect(self.serial_path)
            self.serial.settimeout(0.05)
        except BaseException:
            self.close()            # 启动到一半失败，也不能把 QEMU 留在后台
            raise

    def log_tail(self):
        try:
            with open(self.log_path) as f:
                return f.read().strip()[-300:] or "（没有输出）"
        except OSError:
            return "（读不到日志）"

    def _read_until_prompt(self, timeout=5.0):
        """读到监视器的提示符 (qemu) 为止。超时就报错，不能把半截输出当成结果：
        迟到的输出会被下一条命令当成自己的回答。"""
        data = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            data += chunk.decode(errors="replace")
            if ANSI_ESCAPE.sub("", data).rstrip().endswith("(qemu)"):
                return ANSI_ESCAPE.sub("", data)
        check(self.proc.poll() is None, f"QEMU 已经退出（可能发生了三重错误）：{self.log_tail()}")
        raise TestFailure(f"QEMU 监视器 {timeout:.0f} 秒内没有回到提示符，收到：{ANSI_ESCAPE.sub('', data)[-200:]!r}")

    def cmd(self, line):
        """执行一条监视器命令，返回它的输出。"""
        check(self.proc.poll() is None, f"QEMU 已经退出（可能发生了三重错误）：{self.log_tail()}")
        self.sock.sendall((line + "\n").encode())
        return self._read_until_prompt()

    def read_memory(self, addr, length):
        """读取物理内存。核对每一行的地址，读错位置时报错，而不是悄悄返回错的字节。"""
        out = bytearray()
        while len(out) < length:
            n = min(512, length - len(out))
            start = addr + len(out)
            dump = self.cmd(f"xp /{n}xb {start:#x}")
            chunk = bytearray()
            for line in dump.splitlines():
                m = re.match(r"\s*([0-9a-f]+):\s*((?:0x[0-9a-f]{2}\s*)+)$", line)
                if m:
                    line_addr = int(m.group(1), 16)
                    check(line_addr == start + len(chunk),
                          f"xp 输出的地址 {line_addr:#x} 与期望的 {start + len(chunk):#x} 不符")
                    chunk.extend(int(b, 16) for b in m.group(2).split())
            check(len(chunk) == n, f"xp 只返回了 {len(chunk)} 字节，期望 {n} 字节")
            out.extend(chunk)
        return bytes(out)

    def screen(self, rows=25):
        """读出 VGA 文本显存的前几行，每行去掉行尾空格。"""
        data = self.read_memory(0xB8000, rows * 160)
        lines = []
        for r in range(rows):
            chars = data[r * 160:(r + 1) * 160:2]
            lines.append("".join(chr(c) if 32 <= c < 127 else " " for c in chars).rstrip())
        return lines

    def attribute(self, row, col):
        return self.read_memory(0xB8000 + (row * 80 + col) * 2 + 1, 1)[0]

    def registers(self):
        """解析 info registers 的输出。"""
        text = self.cmd("info registers")
        regs = {}
        for name, value in re.findall(r"\b(E[A-Z]{2}|EFL|CR[0234])=([0-9a-f]{8})", text):
            regs[name] = int(value, 16)
        for name, sel in re.findall(r"^(CS|DS|ES|SS|FS|GS|LDT|TR)\s*=([0-9a-f]{4})", text, re.M):
            regs[name] = int(sel, 16)
        for name, base, limit in re.findall(r"^(GDT|IDT)=\s*([0-9a-f]+) ([0-9a-f]+)", text, re.M):
            regs[name] = (int(base, 16), int(limit, 16))
        m = re.search(r"HLT=(\d)", text)
        regs["HLT"] = int(m.group(1)) if m else None
        return regs

    def gva2gpa(self, addr):
        """把虚拟地址翻译成物理地址；未映射时返回 None。"""
        out = self.cmd(f"gva2gpa {addr:#x}")
        if "Unmapped" in out:
            return None
        m = re.search(r"gpa: (0x[0-9a-f]+)", out)
        check(m is not None, f"gva2gpa 输出无法解析：{out!r}")
        return int(m.group(1), 16)

    def sendkeys(self, keys, delay=0.05):
        for key in keys:
            self.cmd(f"sendkey {key}")
            time.sleep(delay)

    def wait_for(self, predicate, timeout=5.0, what="条件"):
        """反复检查条件。条件里的 check() 失败算作“还没满足”（例如屏幕还没画出来），
        最后一次失败的原因附在超时信息里。QEMU 退出则立刻报错。"""
        deadline = time.time() + timeout
        last = ""
        while time.time() < deadline:
            check(self.proc.poll() is None, f"QEMU 已经退出（可能发生了三重错误）：{self.log_tail()}")
            try:
                if predicate(self):
                    return
            except TestFailure as e:
                if self.proc.poll() is not None:
                    raise
                last = f"（最后一次检查：{e}）"
            time.sleep(0.1)
        raise TestFailure(f"{timeout:.0f} 秒内没有等到{what}{last}")

    def serial_read(self):
        """把串口上已经到了的字符都收进 serial_buf。"""
        try:
            while True:
                chunk = self.serial.recv(4096)
                if not chunk:
                    break
                self.serial_buf += chunk
        except (socket.timeout, BlockingIOError):
            pass

    def serial_wait(self, text, timeout=5, what=None):
        """等串口上出现 text，返回从上次读到的地方到 text 末尾的这一段（解码成字符串），下次从 text 后面接着找。"""
        deadline = time.time() + timeout
        while True:
            self.serial_read()
            i = self.serial_buf.find(text.encode(), self.serial_pos)
            if i >= 0:
                out = self.serial_buf[self.serial_pos:i + len(text)].decode(errors="replace")
                self.serial_pos = i + len(text)
                return out
            if time.time() > deadline:
                rest = self.serial_buf[self.serial_pos:].decode(errors="replace")
                raise TestFailure(f"{timeout:.0f} 秒内串口上没有等到{what or repr(text)}，收到的是：{rest!r}")
            time.sleep(0.02)

    def serial_send(self, text):
        self.serial.sendall(text.encode())

    def close(self):
        try:
            if self.sock:
                self.sock.sendall(b"quit\n")
        except OSError:
            pass
        if self.serial:
            self.serial.close()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class MinixImage:
    """从宿主机这边读 Minix v1 的硬盘镜像，和内核读到的应当一样：超级块、两张位图、i 节点、目录、文件内容。"""

    def __init__(self, path):
        self.img = open(path, "rb").read()
        self.ninodes, self.nzones, self.imap, self.zmap, self.firstdata, _, _, magic = struct.unpack_from("<HHHHHHIH", self.block(1))
        check(magic == 0x137F, f"{path} 不是 Minix v1 文件系统")

    def block(self, n):
        return self.img[n * 1024:(n + 1) * 1024]

    def free_bits(self, first, count, limit):
        data = b"".join(self.block(first + i) for i in range(count))
        return sum(1 for b in range(1, limit + 1) if not data[b // 8] & (1 << (b % 8)))

    def free_inodes(self):
        return self.free_bits(2, self.imap, self.ninodes)

    def free_blocks(self):
        return self.free_bits(2 + self.imap, self.zmap, self.nzones - self.firstdata)

    def inode(self, n):
        off = (n - 1) * 32
        b = self.block(2 + self.imap + self.zmap + off // 1024)
        mode, _, size = struct.unpack_from("<HHI", b, off % 1024)
        return mode, size, struct.unpack_from("<9H", b, off % 1024 + 14)

    def data(self, n):
        """i 节点 n 的内容：直接块、一级间接块、二级间接块，按顺序拼起来，截到文件大小。没分配的块是 0。"""
        mode, size, zones = self.inode(n)
        blocks = list(zones[:7])
        if zones[7]:
            blocks += struct.unpack_from("<512H", self.block(zones[7]))
        if zones[8]:
            for ind in struct.unpack_from("<512H", self.block(zones[8])):
                blocks += struct.unpack_from("<512H", self.block(ind)) if ind else [0] * 512
        out = b"".join(self.block(z) if z else bytes(1024) for z in blocks[:(size + 1023) // 1024])
        return out[:size]

    def lookup(self, path):
        """按路径找 i 节点号，找不到返回 None。"""
        n = 1
        for name in [p for p in path.split("/") if p]:
            data = self.data(n)
            entries = {data[i + 2:i + 16].rstrip(b"\0").decode(): struct.unpack_from("<H", data, i)[0]
                       for i in range(0, len(data), 16) if struct.unpack_from("<H", data, i)[0]}
            if name not in entries:
                return None
            n = entries[name]
        return n

    def read(self, path):
        n = self.lookup(path)
        return None if n is None else self.data(n)


def symbols(elf):
    """用 objdump -t 读出符号表：名称 -> (地址, 大小)。"""
    out = subprocess.run(["objdump", "-t", elf], capture_output=True, text=True, check=True).stdout
    table = {}
    for line in out.splitlines():
        m = re.match(r"([0-9a-f]{8})\s.*\s([0-9a-f]{8})\s+(\S+)$", line)
        if m:
            table[m.group(3)] = (int(m.group(1), 16), int(m.group(2), 16))
    return table


def sections(elf):
    """用 objdump -h 读出各段：名称 -> (地址, 大小)。"""
    out = subprocess.run(["objdump", "-h", elf], capture_output=True, text=True, check=True).stdout
    table = {}
    for line in out.splitlines():
        m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})", line)
        if m:
            table[m.group(1)] = (int(m.group(3), 16), int(m.group(2), 16))
    return table


def check_kernel_loaded(m, elf, kernel_bin):
    """比较内存与 kernel.bin 中的只读部分（.text 和 .rodata）。.data 运行时会被改写，不比较。"""
    with open(kernel_bin, "rb") as f:
        image = f.read()
    table = sections(elf)
    for name in (".text", ".rodata"):
        check(name in table, f"kernel.elf 中没有 {name} 段")
        addr, size = table[name]
        expected = image[addr - 0x10000:addr - 0x10000 + size]
        check(len(expected) == size, f"{name} 超出了 kernel.bin 的范围")
        check(m.read_memory(addr, size) == expected, f"{name}（{addr:#x} 起 {size} 字节）与 kernel.bin 不一致")


def instruction_at(elf, addr):
    """反汇编 addr 处的一条指令，返回助记符。"""
    out = subprocess.run(
        ["objdump", "-d", "--no-show-raw-insn", f"--start-address={addr:#x}",
         f"--stop-address={addr + 8:#x}", elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        m = re.match(rf"\s*{addr:x}:\s+(\S+)", line)
        if m:
            return m.group(1)
    return None


def build_selftest(name):
    """构建一个自检内核，返回它的目录。"""
    build_dir = os.path.join("build", "selftest", name.lower())
    result = subprocess.run(["make", "-s", f"BUILD={build_dir}", f"SELFTEST={name}", "all"],
                            cwd=ROOT, capture_output=True, text=True)
    check(result.returncode == 0, f"构建 SELFTEST={name} 失败：\n{result.stdout}{result.stderr}")
    return os.path.join(ROOT, build_dir)


def in_function(table, name, addr):
    start, size = table[name]
    return start <= addr < start + size


# ---------------------------------------------------------------- 测试用例

USER_LOW, USER_TOP = 0x40000000, 0x40400000   # 用户栈区，与 kernel.h 一致
TSS_SIZE = 104


def tss_sel(n):
    """任务 n 的 TSS 选择子：TSS 描述符从 GDT 第 10 项起排列，与 kernel.h 的 TSS_SEL 一致。"""
    return (10 + n) * 8


def tss_u32(m, elf, n, offset):
    """读任务 n 的 TSS 中偏移 offset 处的 4 字节：4 是 esp0，8 是 ss0，28 是 CR3。"""
    return read_u32(m, symbols(elf)["tss"][0] + n * TSS_SIZE + offset)


def translate(m, cr3, va):
    """按页目录 cr3 手工查两级页表，把虚拟地址翻译成物理地址；没有映射时返回 None。
    监视器的 gva2gpa 只用 CPU 当前的 CR3，查不了别的任务的地址空间。"""
    pde = read_u32(m, cr3 + (va >> 22) * 4)
    if not pde & 1:
        return None
    pte = read_u32(m, (pde & ~0xFFF) + ((va >> 12) & 0x3FF) * 4)
    if not pte & 1:
        return None
    return (pte & ~0xFFF) | (va & 0xFFF)


def boot_ready(m):
    return m.screen(1)[0].startswith("ABPKCVITU")


def test_boot_screen(m, elf):
    """启动后各行的输出"""
    m.wait_for(lambda mm: mm.screen(1)[0] == "ABPKCVITUFWE", timeout=2, what="第 0 行的 ABPKCVITUFWE")
    wait_prompt(m)
    rows = m.screen(10)
    check(rows[1] == "disk: 8192 sectors (4096 KB), QEMU HARDDISK   minix fs: 128 inodes, 4096 blocks",
          f"第 1 行应显示硬盘的扇区数和型号，以及挂载的文件系统：{rows[1]!r}")
    check(rows[2] == "Hello from kmain() in C!", f"第 2 行不对：{rows[2]!r}")
    # 写时复制的次数取决于 fork 之后谁先写栈，不卡死；用掉的页数是 A 的内核栈，init、letter、sh、串口上的 login 各自的
    # 内核栈、页目录、页表、栈页，以及它们用到的程序的页（用到才从硬盘读进来），约 25 页。
    # 缺页至少有 init 的栈页，加上 letter、sh、login 各自用到的代码页和数据页；这几个程序各不相同，没有共用的页
    match = re.fullmatch(r"memory: (\d+)/3840 pages free  page faults: (\d+)  copy-on-write: (\d+)  shared: 0", rows[3])
    check(match and 3800 <= int(match.group(1)) <= 3825 and int(match.group(2)) >= 4 and int(match.group(3)) >= 2,
          f"第 3 行不对：{rows[3]!r}")
    check(re.fullmatch(r"ticks: \d+( +idle: +\d+%)? +\d{4}-\d\d-\d\d \d\d:\d\d:\d\d UTC", rows[4]) is not None, f"第 4 行不对：{rows[4]!r}")
    check(rows[5] == "paging: on, 0xC0000000 -> 0xB8000", f"第 5 行不对：{rows[5]!r}")
    check(rows[6].startswith("tasks: "), f"第 6 行不对：{rows[6]!r}")
    check(rows[8] == "shell on hda: try help, ls /bin, cat /hello.txt, ps", f"第 8 行应是 shell 的欢迎语：{rows[8]!r}")
    check(rows[9] == "$", f"第 9 行应是提示符：{rows[9]!r}")


def test_kernel_in_memory(m, elf):
    """内存中内核的代码和只读数据与 kernel.bin 一致"""
    check_kernel_loaded(m, elf, os.path.join(BUILD, "kernel.bin"))


def test_protected_mode(m, elf):
    """处于 32 位保护模式，GDTR 指向内核自己的 GDT"""
    regs = m.registers()
    check(regs["CR0"] & 1, f"CR0.PE 应为 1，CR0={regs['CR0']:#x}")
    check(regs["CS"] == 0x08, f"CS 应为 0x08，实际是 {regs['CS']:#x}")
    for seg in ("DS", "ES", "FS", "GS", "SS"):
        check(regs[seg] == 0x10, f"{seg} 应为 0x10，实际是 {regs[seg]:#x}")
    gdt_addr, gdt_size = symbols(elf)["gdt"]
    base, limit = regs["GDT"]
    check(base == gdt_addr, f"GDTR 基址应为内核的 gdt {gdt_addr:#x}，实际是 {base:#x}（仍在用 loader 的 GDT？）")
    check(limit == gdt_size - 1, f"GDTR 界限应为 {gdt_size - 1}，实际是 {limit}")


def test_paging(m, elf):
    """分页已打开：CR3 指向页目录，各地址按设计映射"""
    regs = m.registers()
    check(regs["CR0"] & 0x80000000, f"CR0.PG 应为 1，CR0={regs['CR0']:#x}")
    page_directory = symbols(elf)["page_directory"][0]
    check(regs["CR3"] == page_directory, f"CR3 应为 page_directory {page_directory:#x}，实际是 {regs['CR3']:#x}")
    expected = {
        0xC0000000: 0xB8000,        # 显存的第二个地址
        0xC0000004: 0xB8004,
        0xC0001000: None,           # 别名只映射了一页
        0x00000000: None,           # 第 0 页不映射，捕获空指针
        0x00001000: 0x1000,         # 其余前 16 MB 一一对应
        0x00010000: 0x10000,
        0x00FFFFFF: 0xFFFFFF,
        0x01000000: None,           # 16 MB 以上未映射
    }
    for virt, phys in expected.items():
        actual = m.gva2gpa(virt)
        check(actual == phys, f"虚拟地址 {virt:#x} 应映射到 {phys if phys is None else hex(phys)}，实际是 {actual if actual is None else hex(actual)}")


def ticks_value(m):
    match = re.match(r"ticks: (\d+)", m.screen(5)[4])
    check(match is not None, "第 4 行没有 ticks 计数")
    return int(match.group(1))


def idle_percent(m):
    """第 4 行右边每秒更新一次的空闲比例；开始调度不满 1 秒时还没有。"""
    match = re.search(r"idle: +(\d+)%", m.screen(5)[4])
    check(match is not None, f"第 4 行还没有空闲比例：{m.screen(5)[4]!r}")
    return int(match.group(1))


def test_timer_rate(m, elf):
    """定时器约 100 Hz"""
    t1, k1 = time.monotonic(), ticks_value(m)
    time.sleep(2.0)
    t2, k2 = time.monotonic(), ticks_value(m)
    rate = (k2 - k1) / (t2 - t1)
    check(90 <= rate <= 110, f"定时器频率应约为 100 Hz，实测 {rate:.1f} Hz")


KEY_NAMES = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "&": "shift-7", "_": "shift-minus",
             "!": "shift-1", "@": "shift-2", "#": "shift-3", "|": "shift-backslash", ">": "shift-dot", "<": "shift-comma",
             ":": "shift-semicolon", ";": "semicolon", "\\": "backslash", "[": "bracket_left", "]": "bracket_right",
             "'": "apostrophe", '"': "shift-apostrophe", "$": "shift-4", "=": "equal", "+": "shift-equal", "*": "shift-8",
             "(": "shift-9", ")": "shift-0", "%": "shift-5", "\n": "ret"}


def type_text(m, text):
    """把一串字符换成 QEMU 的按键名敲进去：大写字母加 Shift。"""
    keys = []
    for ch in text:
        if ch in KEY_NAMES:
            keys.append(KEY_NAMES[ch])
        elif ch.isupper():
            keys.append("shift-" + ch.lower())
        else:
            keys.append(ch)
    m.sendkeys(keys, delay=0.02)


def bin_programs():
    """硬盘 /bin 里的程序名，按名字排好：与 make 放进 build/rootfs/bin 的相同。"""
    return sorted(os.listdir(os.path.join(BUILD, "rootfs", "bin")))


def ls_wc(names):
    """ls 每行是 类型 1 个字符、大小 8 格、i 节点号 4 格、两个空格、名字，再加换行；wc 数出的行数、词数、字节数。"""
    return f"{len(names)} {4 * len(names)} {sum(16 + len(n) for n in names)}"


def console(m):
    """控制台窗口：第 8～18 行。"""
    return m.screen(19)[8:19]


def last_line(m):
    rows = [r for r in console(m) if r]
    return rows[-1] if rows else ""


def wait_prompt(m):
    """等 shell 在窗口里打出提示符 $（行尾的空格被去掉了），而且光标所在的行只有它。"""
    m.wait_for(lambda mm: last_line(mm) == "$", timeout=5, what="shell 的提示符 $")


def run_command(m, command):
    """在 shell 里敲一条命令，等它运行完、再出现提示符，返回命令那一行之后、新提示符之前的输出。"""
    wait_prompt(m)
    type_text(m, command + "\n")
    def done(mm):
        rows = [r for r in console(mm) if r]
        return len(rows) >= 2 and rows[-1] == "$" and f"$ {command}" in rows
    m.wait_for(done, timeout=5, what=f"命令 {command!r} 运行完")
    rows = [r for r in console(m) if r]
    start = len(rows) - 1 - rows[::-1].index(f"$ {command}")
    return rows[start + 1:-1]


def run_long(m, command):
    """和 run_command 一样，不过命令比窗口宽，折成了几行：按拼起来的样子找它（行尾的空格被去掉了，比较时不看空格）。"""
    wait_prompt(m)
    type_text(m, command + "\n")
    typed = ("$ " + command).replace(" ", "")
    n = (len(command) + 2 + 79) // 80
    def find(rows):
        for i in range(len(rows) - n, -1, -1):
            if "".join(rows[i:i + n]).replace(" ", "") == typed:
                return i
        return None
    def done(mm):
        rows = [r for r in console(mm) if r]
        return len(rows) >= 2 and rows[-1] == "$" and find(rows[:-1]) is not None
    m.wait_for(done, timeout=5, what=f"命令 {command[:30]!r}… 运行完")
    rows = [r for r in console(m) if r]
    return rows[find(rows[:-1]) + n:-1]


def test_keyboard_echo(m, elf):
    """在 shell 里打字：回显、回车把一行交给 shell、退格删到提示符就停"""
    out = run_command(m, "echo hi 42")
    check(out == ["hi 42"], f"echo 应打印 'hi 42'：{out!r}")
    type_text(m, "okx")
    m.sendkeys(["backspace"])
    m.wait_for(lambda mm: last_line(mm) == "$ ok", timeout=2, what="退格删掉 x")
    m.sendkeys(["backspace"] * 4)
    time.sleep(0.3)
    check(last_line(m) == "$", f"退格只能删自己敲的字符，删不到提示符：{last_line(m)!r}")
    m.sendkeys(["ret"])
    wait_prompt(m)


def test_keyboard_shift(m, elf):
    """Shift、右 Shift 和 Caps Lock"""
    wait_prompt(m)
    m.sendkeys(["shift-h", "i", "shift-1", "spc", "shift-2", "shift-minus", "shift_r-q"])
    m.wait_for(lambda mm: last_line(mm) == "$ Hi! @_Q", timeout=2, what="'$ Hi! @_Q'")
    m.sendkeys(["backspace"] * 7 + ["caps_lock", "a", "b", "shift-c", "1", "caps_lock", "d"])
    m.wait_for(lambda mm: last_line(mm) == "$ ABc1d", timeout=2,
               what="'$ ABc1d'（Caps Lock 下 Shift 得到小写，数字不受影响）")
    m.sendkeys(["backspace"] * 5)
    m.wait_for(lambda mm: last_line(mm) == "$", timeout=2, what="删光这一行")


def test_console_scroll(m, elf):
    """控制台窗口（第 8～18 行）写满后向上滚动，窗口外的内容不受影响"""
    before = m.screen(8)
    names = [".", ".."] + bin_programs()
    wait_prompt(m)
    type_text(m, "ls /bin\n")
    # 输出比窗口高，命令那一行和前面几项会滚出去，只能看最后两行：最后一个文件名和新的提示符
    m.wait_for(lambda mm: console(mm)[-1] == "$" and console(mm)[-2].endswith("  " + names[-1]), timeout=5, what="ls /bin 运行完")
    rows = m.screen(25)
    window = rows[8:19]
    check(window[-1] == "$", f"第 18 行应是新的提示符：{window[-1]!r}")
    got = [r.split()[-1] for r in window[:-1]]
    check(got == names[-10:], f"第 8～17 行应是 ls /bin 输出的最后 10 行（命令那一行和前面几项已经滚出窗口）：{window}")
    check(all(r.startswith("-") for r in window[:-1]), f"/bin 下的程序都应是普通文件：{window}")
    for r in (0, 2, 5):
        check(rows[r] == before[r], f"第 {r} 行不应被滚动影响：{before[r]!r} 变成了 {rows[r]!r}")
    check(rows[6].startswith("tasks: ") and rows[4].startswith("ticks: "), "第 4、6 行不应被滚动影响")
    check(all(r == "" for r in rows[19:]), f"第 19～24 行应为空：{rows[19:]}")


def test_idle_in_hlt(m, elf):
    """两个任务都在睡眠时，CPU 交给任务 0，停在 kmain 的 hlt 上；第 4 行的空闲比例接近 100%"""
    table = symbols(elf)
    for _ in range(10):
        regs = m.registers()
        if regs["HLT"] == 1 and regs["TR"] == tss_sel(0) and in_function(table, "kmain", regs["EIP"]):
            check(regs["EFL"] & 0x200, "空闲时 IF 应为 1")
            break
        time.sleep(0.1)
    else:
        raise TestFailure("10 次采样都没有停在任务 0 的 kmain 的 hlt 上：睡眠的任务没有让出 CPU？")
    m.wait_for(lambda mm: idle_percent(mm) >= 90, timeout=3, what="空闲比例达到 90% 以上")


def read_u32(m, addr):
    return int.from_bytes(m.read_memory(addr, 4), "little")


def test_user_setup(m, elf):
    """GDT 有 DPL 3 的用户段；只有用户程序的页打开了用户位"""
    table = symbols(elf)
    gdt_base, _ = m.registers()["GDT"]
    for index, expected, what in ((6, 0x00CFFA000000FFFF, "用户代码段"), (7, 0x00CFF2000000FFFF, "用户数据段")):
        desc = int.from_bytes(m.read_memory(gdt_base + index * 8, 8), "little")
        desc &= ~(1 << 40)          # “已访问”位：CPU 装入段选择子时会自动置 1
        check(desc == expected, f"GDT 第 {index} 项（{what}）应为 {expected:#018x}，实际是 {desc:#018x}")
    pde0 = read_u32(m, table["page_directory"][0])
    check(pde0 & 0x4, f"第 0 个页目录项应打开用户位：{pde0:#x}")
    tables = table["identity_tables"][0]
    user_start, user_data, user_end = (table[n][0] for n in ("__user_start", "__user_data", "__user_end"))
    check(user_start <= table["user_main"][0] < user_data <= user_end, "用户代码应在 __user_start～__user_data")
    for addr in range(user_start, user_end, 4096):
        pte = read_u32(m, tables + (addr >> 12) * 4)
        check(pte & 0x4, f"用户程序的页 {addr:#x} 应打开用户位：{pte:#x}")
        writable = addr >= user_data
        check(bool(pte & 0x2) == writable, f"用户页 {addr:#x} 应{'可写' if writable else '只读'}：{pte:#x}")
    for name in ("kmain", "timer_ticks", "idt", "tss"):
        addr = table[name][0]
        pte = read_u32(m, tables + (addr >> 12) * 4)
        check(not pte & 0x4, f"内核的 {name}（{addr:#x}）所在页不应打开用户位：{pte:#x}")
    vga_pte = read_u32(m, tables + (0xB8000 >> 12) * 4)
    check(not vga_pte & 0x4, f"显存所在页不应打开用户位：{vga_pte:#x}")


def test_user_task(m, elf):
    """任务 B 运行在用户态：它的内核栈顶是从特权级 3 进入系统调用时 CPU 压入的现场"""
    table = symbols(elf)
    top, ss0 = tss_u32(m, elf, 2, 4), tss_u32(m, elf, 2, 8)    # 任务 B 的 TSS 中的 esp0、ss0
    check(ss0 == 0x10, f"任务 B 的 TSS 中 ss0 应为内核数据段 0x10，实际是 {ss0:#x}")
    check(0x100000 < top <= 0x1000000 and top % 4096 == 0, f"任务 B 的内核栈应是分配来的一整页，esp0 = {top:#x}")
    eip, cs, eflags, esp, ss = (read_u32(m, top - 20 + 4 * i) for i in range(5))
    check(cs == 0x33, f"任务 B 进入内核前的 CS 应为用户代码段 0x33，实际是 {cs:#x}")
    check(ss == 0x3B, f"任务 B 进入内核前的 SS 应为用户数据段 0x3B，实际是 {ss:#x}")
    check(in_function(table, "user_main", eip), f"返回地址 {eip:#x} 应在 user_main 内")
    check(USER_TOP - 4096 < esp <= USER_TOP, f"用户态 ESP {esp:#x} 应在用户栈区顶端的那一页内")
    check(eflags & 0x200 and not eflags & 0x3000, f"用户态 EFLAGS 应 IF=1、IOPL=0：{eflags:#x}")


def test_demand_paging(m, elf):
    """每个用户任务有自己的页目录；用户栈按需分配，只有任务 B 用到的栈顶那一页映射了物理页"""
    m.wait_for(lambda mm: mm.screen(1)[0].startswith("ABPKCVITUF"), timeout=2, what="第 0 行的 F")
    table = symbols(elf)
    kernel_dir = table["page_directory"][0]
    cr3 = tss_u32(m, elf, 2, 28)
    check(cr3 != kernel_dir and 0x100000 <= cr3 < 0x1000000, f"任务 B 应有自己的页目录（分配来的一页），CR3 = {cr3:#x}")
    for index in (0, 1, 2, 3, 768):     # 内核部分照抄内核的页目录
        check(read_u32(m, cr3 + index * 4) == read_u32(m, kernel_dir + index * 4), f"第 {index} 个页目录项应与内核的相同")
    check(tss_u32(m, elf, 1, 28) == kernel_dir, "内核任务 A 应使用内核的页目录")
    check(read_u32(m, kernel_dir + (USER_LOW >> 22) * 4) == 0, "内核的页目录里不应有用户栈区")
    pde = read_u32(m, cr3 + (USER_LOW >> 22) * 4)
    check(pde & 7 == 7 and 0x100000 <= pde & ~0xFFF < 0x1000000, f"任务 B 的第 256 个页目录项应指向分配来的页表，存在、可写、用户：{pde:#x}")
    top_page = USER_TOP - 4096
    phys = translate(m, cr3, top_page)
    check(phys is not None and 0x100000 <= phys < 0x1000000, f"栈顶那一页应映射到分配来的物理页，实际是 {phys}")
    pte = read_u32(m, (pde & ~0xFFF) + 1023 * 4) & ~0x60   # CPU 访问后会置上“已访问”“已写”两位
    # fork 出 shell 之后，init 的栈页打着写时复制的记号（第 9 位）、暂时只读，直到它再写这一页
    check(pte in (phys | 7, phys | 0x205), f"页表最后一项应为 物理页 | 7（存在、可写、用户），或者带写时复制记号的只读页：{pte:#x}")
    check(translate(m, cr3, top_page - 4096) is None, "栈顶下面那一页还没用到，不应映射")
    check(translate(m, cr3, USER_LOW) is None, "栈区底部不应映射")


def test_fork_address_spaces(m, elf):
    """fork 出的任务 3 有自己的页目录和内核栈；它 exec 之后，父子的栈顶映射到不同的物理页"""
    m.wait_for(lambda mm: mm.screen(1)[0].startswith("ABPKCVITUFW"), timeout=2, what="第 0 行的 W")
    parent, child = tss_u32(m, elf, 2, 28), tss_u32(m, elf, 3, 28)
    check(child not in (0, parent, symbols(elf)["page_directory"][0]), f"任务 3 应有自己的页目录：CR3 = {child:#x}")
    check(tss_u32(m, elf, 3, 4) not in (0, tss_u32(m, elf, 2, 4)), "任务 3 应有自己的内核栈")
    top = USER_TOP - 4096
    p, c = translate(m, parent, top), translate(m, child, top)
    check(p is not None and c is not None and p != c, f"栈顶应映射到不同的物理页：{p}、{c}")
    pte = read_u32(m, (read_u32(m, child + (USER_LOW >> 22) * 4) & ~0xFFF) + 1023 * 4)
    check(pte & 2, f"任务 3 exec 之后，栈顶是 exec 新分的可写页：{pte:#x}")
    code = symbols(elf)["user_main"][0]
    check(translate(m, parent, code) == translate(m, child, code) == code, "用户代码那一页父子共用，就是内核映像里那一页")


def test_syscall_gate(m, elf):
    """IDT 第 0x80 项是指向 isr_syscall 的 DPL 3 陷阱门"""
    base, limit = m.registers()["IDT"]
    check(limit >= 0x80 * 8 + 7, f"IDT 界限 {limit:#x} 不够容纳第 0x80 项")
    entry = m.read_memory(base + 0x80 * 8, 8)
    offset = entry[0] | entry[1] << 8 | entry[6] << 16 | entry[7] << 24
    selector = entry[2] | entry[3] << 8
    check(entry[5] == 0xEF, f"第 0x80 项的类型属性应为 0xEF（存在、DPL 3、陷阱门），实际是 {entry[5]:#x}")
    check(selector == 0x08, f"第 0x80 项的选择子应为 0x08，实际是 {selector:#x}")
    target = symbols(elf)["isr_syscall"][0]
    check(offset == target, f"第 0x80 项应指向 isr_syscall {target:#x}，实际是 {offset:#x}")


def task_output(m):
    row = m.screen(7)[6]
    check(row.startswith("tasks: "), f"第 6 行不对：{row!r}")
    return row[len("tasks: "):]


OUTPUT_WIDTH = 73                   # 第 6 行 "tasks: " 后面的 73 格，写满后回到开头


def letters_per_tick(m, elf, seconds):
    """一段时间内两个任务一共写了多少个字母，除以经过的 tick 数。
    暂停虚拟机再读输出位置和 timer_ticks，两个值才是同一时刻的。"""
    table = symbols(elf)
    def sample():
        m.cmd("stop")
        try:
            return read_u32(m, table["output_pos"][0]), read_u32(m, table["timer_ticks"][0])
        finally:
            m.cmd("cont")
    p1, k1 = sample()
    time.sleep(seconds)
    p2, k2 = sample()
    check(k2 > k1, "定时器没有走")
    return (p2 - p1) % OUTPUT_WIDTH / (k2 - k1), k2 - k1


def test_tasks_alternate(m, elf):
    """任务写一个字母就去睡、把 CPU 让给别人：第 6 行 A、B、C 逐个轮流，三个任务各每 2 个 tick 写一个"""
    out = task_output(m)
    check(set(out) == {"A", "B", "C"}, f"任务输出中应正好有 A、B 和 fork 出的 C：{out!r}")
    runs = re.findall(r"A+|B+|C+", out)
    # 一行 73 格不是 3 的倍数，写满一圈后新旧字母的交界处会有一次两个相同的字母
    check(len(runs) >= 60 and max(len(r) for r in runs) <= 2,
          f"A、B、C 应逐个轮流（每个任务写一个字母就去睡），实际是 {out!r}")
    rate, ticks = letters_per_tick(m, elf, 0.3)
    check(ticks < OUTPUT_WIDTH * 2 // 3, f"采样间隔 {ticks} 个 tick 太长，输出位置可能已经绕了一圈")
    check(1.2 <= rate <= 1.8, f"三个任务各每 2 个 tick 写一个字母，合计应约 1.5 个/tick，实测 {rate:.2f}（{ticks} 个 tick）")


def test_preemption(_m, _elf):
    """用户程序一直占着 CPU 不睡：时间片用完照样被切走，任务 A 每 20 个 tick 写一个字母，空闲比例接近 0"""
    d = build_selftest("USER_SPIN")
    elf = os.path.join(d, "kernel.elf")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(boot_ready, what="启动完成")
        rate, ticks = letters_per_tick(m, elf, 1.2)
        check(0.03 <= rate <= 0.08,
              f"任务 B 不睡，任务 A 只能在 B 的时间片（20 tick）用完后写一个字母，应约 0.05 个/tick，实测 {rate:.3f}（{ticks} 个 tick）")
        m.wait_for(lambda mm: idle_percent(mm) <= 10, timeout=3, what="空闲比例降到 10% 以下")


PANEL_ROW = 19


def exception_screen(m):
    """异常面板的 5 行（第 19～23 行）。"""
    return m.screen(PANEL_ROW + 5)[PANEL_ROW:]


def panel_fields(panel):
    """把面板中的“名字=0x十六进制”解析成字典。"""
    return {name: int(value, 16) for name, value in re.findall(r"(\w+)=0x([0-9A-F]+)", " ".join(panel))}


def wait_for_panel(m):
    m.wait_for(lambda mm: exception_screen(mm)[0].startswith("Exception"), what="异常面板")
    return exception_screen(m)


def test_divide_error(_m, _elf):
    """除以零：显示 #DE，EIP 指向 div 指令"""
    d = build_selftest("DIVIDE_ERROR")
    with Machine(os.path.join(d, "floppy.img")) as m:
        panel = wait_for_panel(m)
        check(panel[0].startswith("Exception 0 ") and "#DE" in panel[0], f"第 19 行不对：{panel[0]!r}")
        check(m.attribute(PANEL_ROW, 0) == 0x4F and m.attribute(PANEL_ROW + 4, 79) == 0x4F,
              "异常面板应是一整块红底白字")
        fields = panel_fields(panel)
        check("EIP" in fields, f"面板中没有 EIP：{panel}")
        insn = instruction_at(os.path.join(d, "kernel.elf"), fields["EIP"])
        check(insn is not None and insn.startswith("div"), f"EIP 处应是 div 指令，实际是 {insn}")


def test_bad_gate(_m, _elf):
    """触发未填写的 IDT 表项：显示 #GP，错误码 0x30*8+2"""
    d = build_selftest("BAD_GATE")
    with Machine(os.path.join(d, "floppy.img")) as m:
        panel = wait_for_panel(m)
        check(panel[0].startswith("Exception 13 ") and "#GP" in panel[0], f"第 19 行不对：{panel[0]!r}")
        check(panel_fields(panel).get("error") == 0x182, f"错误码应为 0x182：{panel[1]!r}")


def page_fault_test(selftest, error_code, cr2):
    d = build_selftest(selftest)
    with Machine(os.path.join(d, "floppy.img")) as m:
        panel = wait_for_panel(m)
        check(panel[0].startswith("Exception 14 ") and "#PF" in panel[0], f"第 19 行不对：{panel[0]!r}")
        fields = panel_fields(panel)
        check(fields.get("error") == error_code, f"错误码应为 {error_code:#x}：{panel[1]!r}")
        check(fields.get("CR2") == cr2, f"CR2 应为 {cr2:#x}：{panel[4]!r}")


def test_unmapped_read(_m, _elf):
    """读未映射的 0xD0000000：#PF，错误码 0，CR2 为该地址"""
    page_fault_test("UNMAPPED", 0, 0xD0000000)


def test_null_write(_m, _elf):
    """写空指针：#PF，错误码 2（写操作），CR2 为 0"""
    page_fault_test("NULL_WRITE", 2, 0)


def test_clobber_loader(_m, _elf):
    """清零 loader 所在内存后仍能正常响应中断"""
    d = build_selftest("CLOBBER_LOADER")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: mm.screen(1)[0].endswith("Z"), what="清零完成的标记 Z")
        k1 = ticks_value(m)
        time.sleep(1.0)
        k2 = ticks_value(m)
        check(k2 - k1 >= 50, f"清零后定时器应继续工作，1 秒内 ticks 只从 {k1} 变到 {k2}")
        check(not exception_screen(m)[0], f"不应出现异常：{exception_screen(m)[0]!r}")


def test_other_segments(_m, _elf):
    """被打断的代码用别的数据段时，中断入口换成内核段并在返回前恢复"""
    d = build_selftest("OTHER_SEGMENTS")
    elf = os.path.join(d, "kernel.elf")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: mm.screen(1)[0].endswith("N"), what="标记 N")
        k1 = ticks_value(m)
        time.sleep(1.0)
        k2 = ticks_value(m)
        check(k2 - k1 >= 50, f"定时器中断应照常处理，1 秒内 ticks 只从 {k1} 变到 {k2}")
        table = symbols(elf)
        for _ in range(10):
            regs = m.registers()
            if regs["HLT"] == 1 and in_function(table, "selftest_run", regs["EIP"]):
                for seg in ("DS", "ES", "FS", "GS"):
                    check(regs[seg] == 0x48, f"中断返回后 {seg} 应恢复为 0x48，实际是 {regs[seg]:#x}")
                return
            time.sleep(0.1)
        raise TestFailure("10 次采样都没有停在 selftest_run 的 hlt 上")


def test_big_kernel(_m, _elf):
    """内核跨越多个磁道和柱面时，loader 也能完整读入"""
    d = build_selftest("BIG_KERNEL")
    with open(os.path.join(d, "kernel.bin"), "rb") as f:
        kernel = f.read()
    sectors = (len(kernel) + 511) // 512
    check(sectors > 2 * 36, f"自检内核应超过 2 个柱面（72 个扇区），实际只有 {sectors} 个")
    with Machine(os.path.join(d, "floppy.img")) as m:
        # 等内核写出标记：只看长度不行，SeaBIOS 的版本信息也在第 0 行
        m.wait_for(lambda mm: mm.screen(1)[0].startswith("ABPKCVIT") and len(mm.screen(1)[0]) > 12,
                   what="第 0 行第 12 列的标记")
        row0 = m.screen(1)[0]
        check(row0[12] == "Y", f"内核末尾的标记应为 Y，第 0 行是 {row0!r}")
        check_kernel_loaded(m, os.path.join(d, "kernel.elf"), os.path.join(d, "kernel.bin"))


def test_register_dump(_m, _elf):
    """异常面板显示出错那一刻的全部寄存器"""
    d = build_selftest("REGISTER_DUMP")
    table = symbols(os.path.join(d, "kernel.elf"))
    with Machine(os.path.join(d, "floppy.img")) as m:
        panel = wait_for_panel(m)
        check(panel[0].startswith("Exception 6 ") and "#UD" in panel[0], f"第 19 行不对：{panel[0]!r}")
        fields = panel_fields(panel)
        expected = {"EAX": 0x11111111, "EBX": 0x22222222, "ECX": 0x33333333, "EDX": 0x44444444,
                    "ESI": 0x55555555, "EDI": 0x66666666, "EBP": 0x77777777,
                    "EIP": table["selftest_ud2"][0], "CS": 0x08,
                    "DS": 0x10, "ES": 0x10, "FS": 0x10, "GS": 0x10,
                    "CR3": table["page_directory"][0], "error": 0}
        for name, value in expected.items():
            check(fields.get(name) == value, f"{name} 应为 {value:#x}，面板上是 {fields.get(name)}：{panel}")
        stack_bottom, stack_top = table["stack_bottom"][0], table["stack_top"][0]
        check(stack_bottom <= fields.get("ESP", 0) < stack_top,
              f"ESP 应在内核栈 {stack_bottom:#x}～{stack_top:#x} 内，面板上是 {fields.get('ESP')}")
        check(fields.get("EFLAGS", 0) & 0x200, "出错时中断已打开，EFLAGS 的 IF 位应为 1")


def test_syscalls(_m, _elf):
    """各个系统调用的返回值和效果"""
    d = build_selftest("SYSCALLS")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: "slept=" in mm.screen(8)[7], what="第 7 行的系统调用结果")
        rows = m.screen(8)
        fields = dict(re.findall(r"(\w+)=([0-9A-F]+)", rows[7]))
        check(fields.get("pid") == "0", f"kmain 是任务 0，getpid() 应返回 0：{rows[7]!r}")
        check(fields.get("unknown") == "FFFFFFFF", f"未知调用号应返回 -1：{rows[7]!r}")
        check(fields.get("write") == "3", f"write 应返回写出的字节数 3：{rows[7]!r}")
        check(fields.get("badfd") == "FFFFFFFF", f"不支持的 fd 应返回 -1：{rows[7]!r}")
        slept = int(fields.get("slept", "0"))
        check(10 <= slept <= 12, f"sleep(10) 应等待约 10 个 tick，实际 {slept}：{rows[7]!r}")
        check(rows[6].startswith("tasks: xyz"), f"write 的内容应出现在第 6 行开头：{rows[6]!r}")


def test_page_alloc(_m, _elf):
    """物理页从 16 MB 顶端往下分配；释放的页会被重新分出来，交出前已清零；分光 3840 页后再分得到 0"""
    d = build_selftest("PAGE_ALLOC")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: "back=" in mm.screen(8)[7], timeout=10, what="第 7 行的分配结果")
        rows = m.screen(8)
        f = dict(re.findall(r"(\w+)=([0-9A-F]+)", rows[7]))
        check(f.get("free") == "3840", f"开始时 1 MB～16 MB 的 3840 页都应空闲：{rows[7]!r}")
        check(f.get("p1") == "00FFF000" and f.get("p2") == "00FFE000",
              f"应从最高的页往下分配（与 Linux 0.11 相同）：{rows[7]!r}")
        check(f.get("p3") == f.get("p1"), f"释放的页应被重新分出来：{rows[7]!r}")
        check(f.get("zero") == "1", f"重新分出来的页应已清零，不能留着上一个使用者的数据：{rows[7]!r}")
        check(f.get("all") == "3840" and f.get("left") == "0", f"应正好能分出 3840 页，分光后剩 0：{rows[7]!r}")
        check(f.get("back") == "3840", f"全部还回去后应又是 3840 页空闲：{rows[7]!r}")
        check(re.match(r"memory: \d+/3840 pages free", rows[3]), f"第 3 行应显示剩余页数：{rows[3]!r}")


def test_double_free(_m, _elf):
    """同一页释放两次：内核发现自己记错了，显示 Kernel panic 并停机"""
    d = build_selftest("DOUBLE_FREE")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: exception_screen(mm)[0].startswith("Kernel panic"), what="Kernel panic")
        check(exception_screen(m)[0] == "Kernel panic: trying to free free page", f"第 19 行不对：{exception_screen(m)[0]!r}")
        check(m.attribute(PANEL_ROW, 0) == 0x4F, "panic 信息应为红底白字")
        check(m.screen(1)[0][12:13] != "X", "panic 之后不应继续执行")


def test_hd_rw(_m, _elf):
    """硬盘：读出引导块和超级块，最后一个扇区能读、越界的读失败，写进去的扇区能读回来；读写时任务睡下等 IRQ14"""
    d = build_selftest("HD_RW")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: "reqs=" in mm.screen(8)[7], what="第 7 行的硬盘读写结果")
        row = m.screen(8)[7]
        f = dict(re.findall(r"(\w+)=([0-9A-F]+)", row))
        check(f.get("first") == "1", f"第 0 扇区应是 mkfs 写的说明，第 2 扇区应有 Minix 的魔数 0x137F：{row!r}")
        check(f.get("last") == "1", f"最后一个扇区（第 8191 个）应能读：{row!r}")
        check(f.get("beyond") == "FFFFFFFF", f"读第 8192 扇区（超出硬盘）应返回 -1：{row!r}")
        check(f.get("write") == "1", f"写进第 8000 扇区的内容应能原样读回：{row!r}")
        check(f.get("many") == "1", f"连续读 64 个扇区都应成功：{row!r}")
        # 中断叫醒任务后 CPU 正闲着就马上切过去；要是等下一个定时器中断，每个扇区就要多花 10 ms
        check(int(f.get("ticks", "999")) < 10, f"读 64 个扇区应在 0.1 秒内完成：{row!r}")
        # 越界的那次不算；开机时任务 3 也在从硬盘装 /bin/letter，所以只要求不少于这一项自己发出的 69 个
        check(int(f.get("reqs", "0")) >= 69, f"至少应发出 69 个读写请求：{row!r}")
        check(set(task_output(m)) >= {"A", "B", "C"}, f"读完后任务 A 照常运行：{task_output(m)!r}")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")


def message_row(m):
    return m.screen(8)[7]


def check_only_task_a_left(m, what):
    """任务 B 结束后：没有异常面板，定时器照常走，第 6 行很快被任务 A 的字母写满。"""
    check(not exception_screen(m)[0], f"{what}后不应出现异常面板：{exception_screen(m)[0]!r}")
    k1 = ticks_value(m)
    time.sleep(2.5)                 # 任务 A 每秒写约 50 个字母，2.5 秒足够写满一行
    check(ticks_value(m) - k1 >= 200, "任务 B 结束后定时器应照常工作")
    out = task_output(m)
    check(set(out) == {"A"} and len(out) == 73, f"任务 B 结束后第 6 行应只剩 A：{out!r}")
    row3 = m.screen(4)[3]
    check(row3.startswith("memory: 3839/3840 pages free"),
          f"任务 B 结束后它的内核栈、页目录、栈区页表和栈页都应收回，只剩任务 A 的内核栈：{row3!r}")
    for _ in range(10):
        check(m.registers()["TR"] != tss_sel(2), "任务 B 结束后不应再切到它")
        time.sleep(0.03)


def test_user_write_kernel(_m, _elf):
    """用户程序写内核变量：只结束任务 B，消息给出 #PF、错误码 7 和 EIP"""
    d = build_selftest("USER_WRITE_KERNEL")
    table = symbols(os.path.join(d, "kernel.elf"))
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: "killed" in message_row(mm), what="第 7 行的任务结束消息")
        msg = message_row(m)
        check(msg.startswith("task 2 killed by #PF page fault"), f"第 7 行不对：{msg!r}")
        fields = panel_fields([msg])
        check(fields.get("error") == 7, f"错误码应为 7（存在、写、用户态）：{msg!r}")
        check(in_function(table, "user_main", fields.get("EIP", 0)), f"EIP 应在 user_main 内：{msg!r}")
        check(m.attribute(7, 0) == 0x0E, "任务结束的消息应为黄字")
        check_only_task_a_left(m, "用户程序出错")


def test_user_privileged(_m, _elf):
    """用户程序执行 cli：只结束任务 B，消息给出 #GP，EIP 指向 cli"""
    d = build_selftest("USER_PRIVILEGED")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: "killed" in message_row(mm), what="第 7 行的任务结束消息")
        msg = message_row(m)
        check(msg.startswith("task 2 killed by #GP general protection"), f"第 7 行不对：{msg!r}")
        eip = panel_fields([msg]).get("EIP", 0)
        check(instruction_at(os.path.join(d, "kernel.elf"), eip) == "cli", f"EIP 应指向 cli：{msg!r}")
        check_only_task_a_left(m, "用户程序出错")


def test_user_exit(_m, _elf):
    """用户程序调用 exit(7)：任务 B 结束，其余照常"""
    d = build_selftest("USER_EXIT")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: "exited" in message_row(mm), what="第 7 行的任务结束消息")
        msg = message_row(m)
        check(msg == "task 2 exited with code 7", f"第 7 行不对：{msg!r}")
        check_only_task_a_left(m, "exit")


def test_user_bad_pointer(_m, _elf):
    """用户程序把内核地址交给 write：返回 -1"""
    d = build_selftest("USER_BAD_POINTER")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(boot_ready, what="启动完成")
        m.wait_for(lambda mm: "B" in task_output(mm), what="任务 B 的输出")
        out = task_output(m)
        check("K" in out and "X" not in out, f"write 应拒绝内核地址并返回 -1（用户程序写 K），实际输出 {out!r}")


def test_all_tasks_exit(_m, _elf):
    """任务 A、B 都结束后切回任务 0：定时器照常走，CPU 停在 kmain 的 hlt 上"""
    d = build_selftest("ALL_EXIT")
    table = symbols(os.path.join(d, "kernel.elf"))
    with Machine(os.path.join(d, "floppy.img")) as m:
        # 任务 B 先运行、先结束，任务 A 写完 5 个 A 后结束，它的消息写在后面
        m.wait_for(lambda mm: message_row(mm) == "task 1 exited with code 1", what="任务 A 的结束消息")
        k1 = ticks_value(m)
        time.sleep(1.0)
        check(ticks_value(m) - k1 >= 50, "所有任务结束后定时器应照常工作")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")
        check(task_output(m) == "byeAAAAA",
              f"任务 A、B 这一轮的 counter 一样多，先挑任务号大的 B（与 Linux 0.11 相同），B 写完 bye 就结束，A 再写 5 个 A：{task_output(m)!r}")
        for _ in range(10):
            regs = m.registers()
            if regs["HLT"] == 1:
                break
            time.sleep(0.05)
        check(regs["TR"] == tss_sel(0), f"应切回任务 0，实际 TR={regs['TR']:#x}")
        check(m.screen(4)[3].startswith("memory: 3840/3840 pages free"), f"所有任务结束后应收回全部内存：{m.screen(4)[3]!r}")
        check(in_function(table, "kmain", regs["EIP"]), f"应停在 kmain 的 hlt 上，EIP={regs['EIP']:#x}")
        check(regs["CR3"] == table["page_directory"][0], f"CR3 应仍是页目录，实际是 {regs['CR3']:#x}")


def test_early_key(_m, _elf):
    """初始化 8259 之前就有按键：键盘之后仍能正常回显"""
    d = build_selftest("EARLY_KEY")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: mm.screen(1)[0][12:13] == "W", what="等待按键的标记 W")
        m.sendkeys(["x"])
        m.wait_for(boot_ready, what="启动完成")
        m.sendkeys(["h", "i"])
        time.sleep(0.3)
        row = m.screen(9)[8]
        check(row == "hi", f"开中断前的按键被丢弃后，键盘应照常回显（自检内核不拉起 shell）：第 8 行是 {row!r}")


def test_keyboard_edge_cases(_m, _elf):
    """Caps Lock 自动重复只切换一次；退格能跨过自动换行回到上一行"""
    d = build_selftest("KEYBOARD_EDGE")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(boot_ready, what="启动完成")
        rows = m.screen(10)
        expected = "Aa" + "x" * 76 + "ok"
        check(rows[8] == expected, f"第 8 行应为 'Aa' + 76 个 x + 'ok'，实际是 {rows[8]!r}")
        check(rows[9] == "", f"退格应删光换到第 9 行的字符，ok 写回第 8 行末尾：第 9 行是 {rows[9]!r}")


def test_nmi_in_user_mode(_m, _elf):
    """用户态时收到 NMI：不算用户程序的错，显示异常面板，面板上是用户态的 SS 和 ESP"""
    d = build_selftest("USER_SPIN")
    table = symbols(os.path.join(d, "kernel.elf"))
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(boot_ready, what="启动完成")
        for _ in range(100):        # 暂停虚拟机，确认此刻任务 B 在用户态，再注入 NMI
            m.cmd("stop")
            regs = m.registers()
            if regs["TR"] == tss_sel(2) and regs["CS"] == 0x33:
                break
            m.cmd("cont")
            time.sleep(0.03)
        else:
            raise TestFailure("没能在任务 B 处于用户态时暂停虚拟机")
        m.cmd("nmi")
        m.cmd("cont")
        panel = wait_for_panel(m)
        check(panel[0].startswith("Exception 2 ") and "NMI" in panel[0], f"第 19 行不对：{panel[0]!r}")
        fields = panel_fields(panel)
        check(fields.get("CS") == 0x33 and fields.get("SS") == 0x3B, f"NMI 发生在用户态：{panel[1]!r}")
        check(USER_LOW < fields.get("ESP", 0) <= USER_TOP, f"ESP 应是用户栈区中的值：{panel[3]!r}")
        check("killed" not in message_row(m), f"NMI 不应结束任务 B：{message_row(m)!r}")


def test_user_write_code(_m, _elf):
    """用户程序改写自己的代码：代码页只读，只结束任务 B"""
    d = build_selftest("USER_WRITE_CODE")
    table = symbols(os.path.join(d, "kernel.elf"))
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: "killed" in message_row(mm), what="第 7 行的任务结束消息")
        msg = message_row(m)
        check(msg.startswith("task 2 killed by #PF page fault"), f"第 7 行不对：{msg!r}")
        fields = panel_fields([msg])
        check(fields.get("error") == 7, f"错误码应为 7（页存在、写、用户态）：{msg!r}")
        check(in_function(table, "user_main", fields.get("EIP", 0)), f"EIP 应在 user_main 内：{msg!r}")
        check_only_task_a_left(m, "用户程序改写代码")


def test_deep_stack(_m, _elf):
    """用户程序用掉 5 页栈：每页第一次用到时补上，内核读到没映射的栈页也补上；exit 后全部收回"""
    d = build_selftest("USER_DEEP_STACK")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 3", what="任务 B 的结束消息")
        check("k" in task_output(m) and "x" not in task_output(m),
              f"write 读到栈区里没映射的页时内核应补上，返回 1（用户程序写 k）：{task_output(m)!r}")
        row3 = m.screen(4)[3]
        match = re.fullmatch(r"memory: (\d+)/3840 pages free  page faults: (\d+)  copy-on-write: \d+  shared: \d+", row3)
        check(match is not None, f"第 3 行不对：{row3!r}")
        check(int(match.group(2)) >= 5, f"5 页栈应各缺页一次：{row3!r}")
        check(match.group(1) == "3839", f"任务 B 结束后它的页应全部收回，只剩任务 A 的内核栈：{row3!r}")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")


def test_fork_isolation(_m, _elf):
    """fork 后子进程改栈上的变量，父进程看到的仍是原值；fork 在父进程返回 3、在子进程返回 0；结束后内存全部收回"""
    d = build_selftest("USER_FORK")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", what="父进程的结束消息")
        out = task_output(m)
        check("c" in out and "x" not in out, f"子进程应看到自己改过的值（写 c）：{out!r}")
        check("p" in out and "X" not in out, f"父进程的变量不应被子进程改掉（写 p）：{out!r}")
        check(out.index("c") < out.index("p"), f"父进程等了 10 个 tick，子进程应先写：{out!r}")
        check("p3" in out, f"fork 在父进程中应返回子进程的任务号 3：{out!r}")
        m.wait_for(lambda mm: mm.screen(4)[3].startswith("memory: 3839/3840 pages free"), timeout=2,
                   what="父子进程的内存全部收回（只剩任务 A 的内核栈）")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")


def test_waitpid(_m, _elf):
    """waitpid：取回退出码；内核写 fork 共享的页也会先复制；任务槽用光时 fork 返回 -1；WNOHANG；被异常结束的子进程"""
    d = build_selftest("USER_WAIT")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=10, what="父进程的结束消息")
        out = task_output(m).replace("A", "")
        expected = "winrhk"
        meaning = {"w": "waitpid 返回子进程号并写回退出码 42", "i": "内核写共享页时先复制，另一个进程的那份不变",
                   "n": "16 个任务槽里能 fork 13 次，第 14 次返回 -1", "r": "waitpid(-1) 收走 13 个子进程，退出码都是 7",
                   "h": "WNOHANG 立刻返回 0，等别人的子进程返回 -1", "k": "被缺页异常结束的子进程，状态是 0x10E"}
        for i, ch in enumerate(expected):
            check(len(out) > i and out[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：第 6 行去掉 A 后是 {out!r}，应为 {expected!r}")
        m.wait_for(lambda mm: mm.screen(4)[3].startswith("memory: 3839/3840 pages free"), timeout=2,
                   what="所有子进程的内存和内核栈都收回（只剩任务 A 的内核栈）")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")


def test_file_system(_m, _elf):
    """Minix 文件系统：open/read/close 读文件、600 KB 的大文件、目录和子目录；描述符用光；fork 后共用读写位置"""
    d = build_selftest("USER_FS")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=15, what="任务 B 的结束消息")
        out = task_output(m)
        hello = "Hello from the Minix file system on hda!"
        check(out.startswith(hello), f"第 6 行开头应是 /hello.txt 的内容：{out!r}")
        expected = "oubdscf"
        meaning = {"o": "不存在的文件和过长的路径，open 返回 -1", "u": "缓冲区是内核地址，read 返回 -1",
                   "b": "600 KB 的 /big 逐字节正确，读到末尾返回 0", "d": "根目录里有 hello.txt、big、etc",
                   "s": "读出子目录里的 /etc/issue", "c": "close 后 read 返回 -1，描述符用光后 open 返回 -1",
                   "f": "fork 后父子共用读写位置"}
        got = out[len(hello):]
        for i, ch in enumerate(expected):
            check(len(got) > i and got[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：检查结果是 {got!r}，应为 {expected!r}")
        check(m.screen(2)[1].endswith("minix fs: 128 inodes, 4096 blocks"), f"挂载后第 1 行应显示文件系统的大小：{m.screen(2)[1]!r}")
        table = symbols(os.path.join(d, "kernel.elf"))
        hits, misses = read_u32(m, table["buffer_hits"][0]), read_u32(m, table["buffer_misses"][0])
        # /big 有 600 个数据块，加上间接块、i 节点表、目录，未命中应略多于 600；反复打开 /hello.txt 时目录和 i 节点都在缓存里
        check(600 < misses < 650, f"缓冲区缓存：每个块只应从硬盘读一次，未命中 {misses} 次")
        check(hits > 100, f"缓冲区缓存：反复查目录、读 i 节点应命中，命中 {hits} 次")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")


def test_exec(_m, _elf):
    """exec：失败时原程序接着运行；/bin/hello 拿到参数；/bin/check 的 .data、.bss、argv 都对；写代码页被结束"""
    d = build_selftest("USER_EXEC")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=10, what="任务 B 的结束消息")
        out = task_output(m)
        expected = "o" + "hello from exec" + "h" + "check ok" + "k" + "c"
        check(out == expected, f"第 6 行应为 {expected!r}（o：exec 失败时返回 -1；hello 打印参数后以 0 退出；"
                               f"check 检查自己的段后以 7 退出；check crash 写代码页，状态 0x10E），实际是 {out!r}")
        check(m.screen(1)[0][11:12] == "E", f"第一次 exec 成功时第 0 行第 11 列应出现 E：{m.screen(1)[0]!r}")
        m.wait_for(lambda mm: mm.screen(4)[3].startswith("memory: 3840/3840 pages free"), timeout=2,
                   what="所有程序结束后内存全部收回")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")


def test_pipe(_m, _elf):
    """pipe 和 dup：读写；10000 个字节经过一页的管道，写满了就等；写的一头全关后读到 0；读的一头关了写返回 -1"""
    d = build_selftest("USER_PIPE")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=10, what="任务 B 的结束消息")
        out = task_output(m)
        check(out == "pbwed", f"第 6 行应为 pbwed（p 读写、b 大量数据和读到末尾、w 子进程退出、e 没有读者时写失败、d dup），实际是 {out!r}")
        m.wait_for(lambda mm: mm.screen(4)[3].startswith("memory: 3840/3840 pages free"), timeout=2,
                   what="管道的缓冲区页随两头关闭一起收回")


def test_nice(_m, _elf):
    """nice 和 times：nice 值的范围和继承；nice 0 和 nice 19 抢 CPU 按 20:1 分；两个 nice 0 平分；utime 和 stime 分开记；cutime、cstime"""
    d = build_selftest("USER_NICE")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=25, what="init 的结束消息")
        out = task_output(m).replace("A", "")
        expected = "nidfts"
        meaning = {"n": "nice 返回新的 nice 值，停在 -20～19；priority 是 20 - nice", "i": "fork 和 exec 之后 nice 值不变",
                   "d": "nice 0 和 nice 19 的两个空转的进程，CPU 至少按 8:1 分（算法是 20:1），nice 19 的没饿死",
                   "f": "两个 nice 0 的空转进程分得差不多", "t": "用户态空转记在 utime，一直调用 stat 的大半记在 stime",
                   "s": "收走的子进程用的时间加进了父进程的 cutime、cstime"}
        for i, ch in enumerate(expected):
            check(len(out) > i and out[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：第 6 行去掉 A 后是 {out!r}，应为 {expected!r}")


def test_permissions(_m, _elf):
    """用户和权限：用户号和 setuid；读写执行权限；在谁的目录里能新建、删除；/tmp 的粘滞位；chmod、chown、utime；kill 和 nice；setuid 程序"""
    d = build_selftest("USER_PERM")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=15, what="init 的结束消息")
        out = task_output(m).replace("A", "")
        expected = "urcsxmke!"
        meaning = {"u": "init 是 root；setgid、setuid(100) 以后四个号都是 100，回不去", "r": "读写权限：读得了的、写不了的、O_TRUNC、0700 的目录",
                   "c": "root 的目录里建不了文件、目录、链接；/tmp 里能建，主人是 100", "s": "/tmp 的粘滞位：删不掉 root 的文件，能删自己的",
                   "x": "0644 的程序执行不了；0700 的目录里 stat 不到、进不去", "m": "chmod 只有主人能改；chown 只有 root；utime 的两种情况",
                   "k": "kill 不了 root 的进程、能 kill 自己的；nice 调低不了", "e": "setuid root 的 /bin/id 打印 euid=0(root)",
                   "!": "回到 root：能删用户的文件；root 也执行不了没有执行位的程序，有了就能；能进 0700 的目录；chown 清掉 setuid 位"}
        for i, ch in enumerate(expected):
            check(len(out) > i and out[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：第 6 行去掉 A 后是 {out!r}，应为 {expected!r}")


def test_no_disk(_m, _elf):
    """不接硬盘开机：第 1 行显示 disk: none；exec 不了 /bin/letter，任务 3 用内核映像里的代码写 C；init 隔一会儿才重试 shell"""
    tmp = tempfile.mkdtemp(prefix="kernel-nodisk-")
    image = os.path.join(tmp, "floppy.img")
    with open(os.path.join(BUILD, "floppy.img"), "rb") as src, open(image, "wb") as dst:
        dst.write(src.read())
    with Machine(image) as m:
        m.wait_for(boot_ready, what="启动完成")
        check(m.screen(2)[1] == "disk: none", f"第 1 行应为 disk: none：{m.screen(2)[1]!r}")
        m.wait_for(lambda mm: {"A", "B", "C"} <= set(task_output(mm)), timeout=3, what="第 6 行的 A、B、C")
        check(m.screen(1)[0][11:12] != "E", "没有硬盘，不应有程序被 exec 装入")
        # 每拉起一次 shell 都要 fork，就有写时复制；隔 1 秒才重试的话，2 秒里只会多几次
        cow = lambda: int(re.search(r"copy-on-write: (\d+)", m.screen(4)[3]).group(1))
        c1 = cow()
        time.sleep(2.0)
        c2 = cow()
        check(c2 - c1 < 30, f"shell 起不来时 init 应隔一会儿再试：2 秒里写时复制从 {c1} 涨到 {c2}")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")


def test_signal_syscalls(_m, _elf):
    """kill 和 signal：结束睡着的子进程、忽略、SIGKILL 不能忽略、进程组、setsid、打断管道和 waitpid、SIGPIPE、参数检查、僵尸、处理函数、exec 后的处理方式"""
    d = build_selftest("USER_SIGNAL")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=15, what="init 的结束消息")
        out = task_output(m).replace("A", "")
        expected = "tikgspwevzhrxbuq"
        meaning = {"t": "kill 打断 sleep，子进程被 SIGTERM 结束，状态 0x20F", "i": "忽略了 SIGTERM 的子进程照常退出",
                   "k": "SIGKILL 不能忽略", "g": "kill(0, …) 发给自己这一组，忽略随 fork 继承",
                   "s": "setsid 之后自成一组，kill(-组号, …) 才发得到", "p": "睡在管道上的读者、写者都能被结束",
                   "w": "睡在 waitpid 里的进程能被结束", "e": "往没人读的管道里写：被 SIGPIPE 结束，忽略了就返回 -1", "v": "kill、signal 的参数不对返回 -1，内核任务和 init 收不到信号",
                   "z": "给僵尸发信号返回 0，退出码不变", "h": "处理函数拿到信号编号，被打断的 kill 照样返回 0，第二次还有效",
                   "r": "设了处理函数的信号打断管道上的 read：返回 -EINTR，程序接着运行",
                   "x": "exec 之后处理函数改回默认，忽略的保持忽略", "b": "处理函数不在用户的内存里，signal 返回 SIG_ERR",
                   "u": "在用户态空转的进程也能被 kill 结束（定时器中断返回时处理信号）",
                   "q": "处理函数前后 eax、ecx、edx、DF 原样恢复，处理函数里 DF 是 0"}
        for i, ch in enumerate(expected):
            check(len(out) > i and out[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：第 6 行去掉 A 后是 {out!r}，应为 {expected!r}")
        m.wait_for(lambda mm: mm.screen(4)[3].startswith("memory: 3839/3840 pages free"), timeout=2,
                   what="所有子进程的内存和内核栈都收回（只剩任务 A 的内核栈）")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")


def test_brk(_m, _elf):
    """brk：堆从程序末尾开始、用到才补页；堆和栈之间不补页；fork 继承；越界的 brk 不动；exec 之后从新程序的末尾开始；栈区边界；sort 和 malloc"""
    d = build_selftest("USER_BRK")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=10, what="init 的结束消息")
        out = task_output(m).replace("A", "")
        expected = "qhgfleoksnm"
        meaning = {"q": "init 的堆从 0x40000000 开始", "h": "brk 往上挪 8 KB，写满再读回来",
                   "g": "堆和栈之间没要过的地方一写就被结束，brk 所在的那一页能用", "f": "fork 之后堆一样大、内容一样，各改各的",
                   "l": "低于程序末尾或离栈区不到 16 KB 的 brk 不动，能缩回空堆", "e": "系统调用的缓冲区在没要过的地方或者跨过堆末尾，read 返回 -1",
                   "o": "exec 之后堆从新程序的 .bss 末尾开始，malloc 来的内存能用",
                   "k": "栈区是顶端 64 KB；堆推不到紧贴栈区，栈用超了被结束", "s": "sort 按字节排序（含 0、0x80、0xFF），-r 倒序",
                   "n": "sort 读两个文件，前一个末行没换行也不接在一起", "m": "malloc 的检查：合并、和 sbrk 混用、realloc、随机分配"}
        for i, ch in enumerate(expected):
            check(len(out) > i and out[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：第 6 行去掉 A 后是 {out!r}，应为 {expected!r}")
        m.wait_for(lambda mm: mm.screen(4)[3].startswith("memory: 3839/3840 pages free"), timeout=2,
                   what="堆的页随进程结束一起收回（只剩任务 A 的内核栈）")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")


def test_alarm(_m, _elf):
    """alarm 和 pause：闹钟大约 1 秒后响；剩几秒和取消；pause 等到信号；没设处理函数就被结束；fork 不继承；exec 保留"""
    d = build_selftest("USER_ALARM")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=15, what="init 的结束消息")
        out = task_output(m).replace("A", "")
        expected = "crpkfe"
        meaning = {"c": "alarm(1) 大约 1 秒后发 SIGALRM，处理函数接住，正在睡的 sleep 被打断",
                   "r": "alarm 返回还剩几秒，alarm(0) 取消", "p": "pause 等到闹钟，返回 -EINTR",
                   "k": "没设处理函数的进程被 SIGALRM 结束", "f": "fork 出的子进程没有父进程的闹钟",
                   "e": "exec 之后闹钟还在"}
        for i, ch in enumerate(expected):
            check(len(out) > i and out[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：第 6 行去掉 A 后是 {out!r}，应为 {expected!r}")
        m.wait_for(lambda mm: mm.screen(4)[3].startswith("memory: 3839/3840 pages free"), timeout=2,
                   what="子进程的内存都收回（只剩任务 A 的内核栈）")


def test_sync(_m, _elf):
    """延迟写回：新建文件时一块也不写盘；sync 正好写回那么多块；写 40 KB 时换块写回；从硬盘读回来逐字节核对；删掉后 df 回到原样"""
    d = build_selftest("USER_SYNC")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=15, what="init 的结束消息")
        out = task_output(m).replace("A", "")
        expected = "dsewr"
        meaning = {"d": "新建 3 KB 的文件：多了脏块，硬盘上一块也没写", "s": "sync 把脏块都写回，块数对得上；再 sync 不用写",
                   "e": "写 40 KB：换块时脏块先写回，没 sync 也写了", "w": "读回来的内容逐字节对（换出去的块写回了硬盘）",
                   "r": "删掉再 sync：块和 i 节点都收回，没有脏块"}
        for i, ch in enumerate(expected):
            check(len(out) > i and out[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：第 6 行去掉 A 后是 {out!r}，应为 {expected!r}")


def test_sync_to_disk(_m, _elf):
    """sync 真的写到了硬盘上：在镜像的副本上运行，sync 之后直接杀掉 QEMU，从宿主机读镜像，文件内容、目录、空闲块数都对"""
    d = tempfile.mkdtemp(prefix="kernel-disk-")
    for f in ("floppy.img", "hd.img", "hdb.img"):
        shutil.copy(os.path.join(BUILD, f), d)
    text = "persisted on disk"
    with Machine(os.path.join(d, "floppy.img"), snapshot=False) as m:
        m.wait_for(boot_ready, what="开机")
        for command in ("mkdir /tmp/keep", f"echo {text} > /tmp/keep/a", "cat /hello.txt /big > /tmp/keep/big",
                        "mount /dev/hdb /mnt", "echo on hdb > /mnt/b", "sync"):
            check(run_command(m, command) == [], f"{command} 不打印东西")
        used = run_command(m, "df")[0]
        m.proc.kill()               # 不让 QEMU 好好退出、也不让 init 再 sync：镜像里有的，就是 sync 写下去的
        m.proc.wait()
    img = MinixImage(os.path.join(d, "hd.img"))
    check(img.read("/tmp/keep/a") == (text + "\n").encode(), f"镜像里 /tmp/keep/a 的内容：{img.read('/tmp/keep/a')!r}")
    big = img.read("/tmp/keep/big")
    want = img.read("/hello.txt") + img.read("/big")
    check(big is not None and len(big) == len(want) and big == want,
          f"镜像里 /tmp/keep/big（用到二级间接块）和 /hello.txt 加 /big 逐字节一样：{None if big is None else len(big)} 字节，应为 {len(want)}")
    hdb = MinixImage(os.path.join(d, "hdb.img"))
    check(hdb.read("/b") == b"on hdb\n" and hdb.read("/readme") is not None and img.read("/b") is None,
          f"写进 /mnt/b 的内容在 hdb 的镜像里，不在 hda 上：{hdb.read('/b')!r}")
    g = re.fullmatch(r"/: blocks: (\d+) used, (\d+) free; inodes: (\d+) used, (\d+) free", used)
    check(g and int(g.group(2)) == img.free_blocks() and int(g.group(4)) == img.free_inodes(),
          f"镜像里的两张位图和 df 说的空闲块数、空闲 i 节点数一样：df {used!r}，镜像 {img.free_blocks()} 块、{img.free_inodes()} 个 i 节点")
    shutil.rmtree(d)


def test_link(_m, _elf):
    """硬链接：两个名字一个 i 节点、链接数 2；从一个名字写另一个马上读得到；删一个名字不收回；开着的文件删光名字也能读；错误情况；链接数上限；末尾的 /；mv 的长名字"""
    d = build_selftest("USER_LINK")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=10, what="init 的结束消息")
        out = task_output(m).replace("A", "")
        expected = "lwuoemtv"
        meaning = {"l": "link 之后同一个 i 节点、链接数 2、内容一样", "w": "从一个名字写，另一个名字马上读得到",
                   "u": "删掉一个名字不收回，链接数变回 1；两个都删了才收回", "o": "开着的文件删光名字也能读，关掉才收回",
                   "e": "目录、已经存在的名字（包括 . 和 ..）、不存在的源和目录、太长的名字都不行",
                   "m": "一个文件最多 250 个名字，再 link 返回 -1，链接数不回绕",
                   "t": "文件名后面加 / 打不开、删不掉、不能新建和链接；目录末尾带 / 照常可以",
                   "v": "mv 拼出来的名字太长时报错，不截短、不删掉别的文件"}
        for i, ch in enumerate(expected):
            check(len(out) > i and out[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：第 6 行去掉 A 后是 {out!r}，应为 {expected!r}")


def test_time_syscalls(_m, _elf):
    """时间：日期换算（闰年、2100 年、2106 年）、time 往前走、文件和目录的修改时间、utime、文件的主人"""
    d = build_selftest("USER_TIME")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=10, what="init 的结束消息")
        out = task_output(m).replace("A", "")
        expected = "ctmdu"
        meaning = {"c": "日期和秒数互相换算", "t": "time 从 CMOS 的时钟往后数", "m": "文件的修改时间、utime",
                   "d": "目录里增删一项，目录的修改时间变成现在", "u": "文件的主人和组是 0"}
        for i, ch in enumerate(expected):
            check(len(out) > i and out[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：第 6 行去掉 A 后是 {out!r}，应为 {expected!r}")


def test_fcntl(_m, _elf):
    """dup2 和 fcntl：共用读写位置、换掉开着的描述符、F_DUPFD、F_GETFL/F_SETFL、close-on-exec 在 exec 时关掉、fork 继承"""
    d = build_selftest("USER_FCNTL")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=10, what="init 的结束消息")
        out = task_output(m).replace("A", "")
        expected = "dfcke"
        meaning = {"d": "dup2", "f": "F_DUPFD、F_GETFL、F_SETFL", "c": "close-on-exec：exec 时关掉", "k": "fork 继承 close-on-exec",
                   "e": "不对的描述符和命令返回 -1"}
        for i, ch in enumerate(expected):
            check(len(out) > i and out[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：第 6 行去掉 A 后是 {out!r}，应为 {expected!r}")


def test_fs_write(_m, _elf):
    """写文件：O_EXCL、读写权限、O_TRUNC、O_APPEND、删掉开着的文件、目录的错误、同一个文件开两次共用 i 节点"""
    d = build_selftest("USER_FSWRITE")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 0", timeout=10, what="任务 B 的结束消息")
        out = task_output(m)
        expected = "emtuds"
        meaning = {"e": "O_EXCL", "m": "读写权限", "t": "覆盖、O_TRUNC、O_APPEND", "u": "删掉开着的文件，关掉后才收回",
                   "d": "目录的各种错误", "s": "两个描述符共用 i 节点"}
        for i, ch in enumerate(expected):
            check(len(out) > i and out[i] == ch, f"第 {i + 1} 项（{meaning[ch]}）没有通过：检查结果是 {out!r}，应为 {expected!r}")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")


def test_wild_read(_m, _elf):
    """用户程序读栈区以外没映射的地址：内核不补页，只结束任务 B，错误码 4"""
    d = build_selftest("USER_WILD_READ")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: "killed" in message_row(mm), what="第 7 行的任务结束消息")
        msg = message_row(m)
        check(msg.startswith("task 2 killed by #PF page fault"), f"第 7 行不对：{msg!r}")
        check(panel_fields([msg]).get("error") == 4, f"错误码应为 4（页不存在、读、用户态）：{msg!r}")
        check_only_task_a_left(m, "用户程序读野指针")


def test_shell(m, elf):
    """shell：cat、找不到的命令、check、被异常结束的程序、ps、后台运行、exit 后 init 再拉起 shell；命令结束后内存收回"""
    out = run_command(m, "cat /hello.txt")
    check(out == ["Hello from the Minix file system on hda!"], f"cat 应打印 /hello.txt：{out!r}")
    out = run_command(m, "help")
    progs = [p for p in bin_programs() if p != "sh"]
    heads = ["files: ", "disks: ", "tasks: ", "users: "]
    listed = [p for r in out[:4] for p in r.split()[1:]] if len(out) == 6 else []
    check(len(out) == 6 and all(r.startswith(h) for r, h in zip(out, heads)) and sorted(listed) == progs and all(len(r) < 80 for r in out),
          f"help 应是 6 行，每行都放得下（不折行），头四行合起来列出 /bin 里除了 sh 的每个程序 {progs}：{out!r}")
    free_before = m.screen(4)[3].split("/")[0]
    check(run_command(m, "nosuch") == ["sh: nosuch: not found"], "找不到的命令应报告 not found")
    check(run_command(m, "check") == ["check ok"], "check 应通过")
    check(run_command(m, "check badptr") == ["badptr ok"],
          "把代码页的地址交给要写用户内存的系统调用，应逐个返回 -1，而不是让内核停机")
    words = " ".join(str(i) for i in range(1, 18))
    check(run_command(m, "echo " + words) == [" ".join(str(i) for i in range(1, 16))],
          "shell 最多 16 个参数，多余的不要，不能并进最后一个参数里")
    out = run_command(m, "check crash")
    check(out == ["sh: check killed by exception 14"], f"check crash 写代码页，应被缺页异常结束：{out!r}")
    out = run_command(m, "ps")
    check(out[0] == "PID PPID S NAME", f"ps 的表头不对：{out!r}")
    rows = {r.split()[0]: r.split() for r in out[1:]}
    # 任务 1～3 每写一个字母睡 2 个 tick：ps 查到它们时多半在睡（S）；也可能刚被定时器叫醒，在等 ps 用完时间片（R）
    expected = {"0": ["0", "0", "R", "idle"], "1": ["1", "0", "SR", "task_a"], "2": ["2", "0", "SR", "init"],
                "3": ["3", "2", "SR", "letter"], "4": ["4", "2", "W", "sh"]}
    for pid, (want_pid, want_ppid, states, name) in expected.items():
        row = rows.get(pid)
        check(row is not None and len(row) == 4 and row[0] == want_pid and row[1] == want_ppid and row[2] in states and row[3] == name,
              f"ps 中任务 {pid} 应为 {want_pid} {want_ppid} {'或'.join(states)} {name}：{out!r}")
    check(any(r[3] == "ps" and r[2] == "R" and r[1] == "4" for r in rows.values()), f"ps 应列出自己：{out!r}")
    m.wait_for(lambda mm: mm.screen(4)[3].split("/")[0] == free_before, timeout=2,
               what=f"命令都结束后空闲页回到 {free_before}")

    names = [".", ".."] + bin_programs()
    check(run_command(m, "ls /bin | wc") == [ls_wc(names)], f"ls /bin 的输出经管道交给 wc，应为 {ls_wc(names)}")
    check(run_command(m, "cat /hello.txt | grep Minix") == ["Hello from the Minix file system on hda!"], "grep 从管道读")
    out = run_command(m, "ps | grep sh")
    # ps 运行时，grep 那个子进程可能还没来得及 exec，名字仍是 sh，所以可能多出一行
    check(" 4    2 W sh" in out and all(l.endswith(" sh") for l in out) and len(out) <= 2, f"ps 的输出经管道交给 grep：{out!r}")
    with_e = [n for n in names if "e" in n]
    check(run_command(m, "ls /bin | grep e | wc") == [ls_wc(with_e)], f"三段的管道：含 e 的 {len(with_e)} 个文件名")
    check(run_command(m, "cat /big | wc") == ["2400 7201 614400"], "600 KB 经过只有一页的管道，写满了就等，一个字节也不丢")
    wait_prompt(m)
    type_text(m, "wc\nhello world\n")
    m.wait_for(lambda mm: last_line(mm) == "hello world", timeout=2, what="wc 从键盘读")
    m.sendkeys(["ctrl-d"])
    m.wait_for(lambda mm: console(mm)[-2:] == ["1 2 12", "$"], timeout=2, what="Ctrl+D 结束输入，wc 打印 1 2 12")
    m.wait_for(lambda mm: mm.screen(4)[3].split("/")[0] == free_before, timeout=2,
               what=f"管道和命令都结束后空闲页回到 {free_before}")

    df = run_command(m, "df")
    check(len(df) == 2 and df[0].startswith("/: blocks: ") and df[1].startswith("cache: "), f"df 的输出不对：{df!r}")
    df = df[0]                      # 第二行是缓冲区缓存的数字，随时在变
    for command, expected in [("echo hello > /tmp/a", []), ("echo world >> /tmp/a", []), ("cat /tmp/a", ["hello", "world"]),
                              ("wc < /tmp/a", ["2 2 12"]), ("mkdir /tmp/d", []), ("echo x > /tmp/d/f", []),
                              ("rmdir /tmp/d", ["rmdir: /tmp/d: cannot remove (not an empty directory?)"]),
                              ("rm /tmp/d/f", []), ("rmdir /tmp/d", []), ("rm /tmp/a", []), ("cat /tmp/a", ["cat: /tmp/a: cannot open"])]:
        out = run_command(m, command)
        check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    wait_prompt(m)
    type_text(m, "cat /big > /tmp/big2\n")
    m.wait_for(lambda mm: last_line(mm) == "$" and "$ cat /big > /tmp/big2" in console(mm), timeout=20,
               what="把 600 KB 的 /big 复制成 /tmp/big2")
    check(run_command(m, "cat /tmp/big2 | wc") == ["2400 7201 614400"], "复制出的文件要用到二级间接块，内容应完全一样")
    used = run_command(m, "df")[0]
    check(run_command(m, "rm /tmp/big2") == [], "rm 应成功")
    check(run_command(m, "df")[0] == df, f"删掉之后，块和 i 节点都应收回：复制后是 {used!r}，删掉后应回到 {df!r}")

    out = run_command(m, "letter D &")
    check(len(out) == 1 and re.fullmatch(r"\[\d+\]", out[0]), f"后台运行应打印任务号：{out!r}")
    m.wait_for(lambda mm: "D" in task_output(mm), timeout=2, what="第 6 行出现后台的 letter 写的 D")

    wait_prompt(m)
    type_text(m, "exit\n")
    m.wait_for(lambda mm: "shell on hda: try help, ls /bin, cat /hello.txt, ps" in console(mm)[-2:], timeout=5,
               what="shell 退出后 init 再拉起一个新的 shell")
    wait_prompt(m)
    out = run_command(m, "ps")
    names = [r.split()[-1] for r in out[1:]]
    check(names.count("sh") == 1 and "letter" in names, f"新的 shell 应只有一个，后台的 letter 还在：{out!r}")



def test_cwd(m, elf):
    """当前目录：cd、pwd、相对路径、. 和 ..，子进程继承当前目录，新的 shell 从根目录开始"""
    names = lambda out: [line.split()[-1] for line in out]
    for command, expected in [("pwd", ["/"]), ("cd /etc", []), ("pwd", ["/etc"]),
                              ("cat issue", ["Linux 0.11 learning kernel"]),
                              ("cd ..", []), ("pwd", ["/"]), ("cd /bin/../etc/./", []), ("pwd", ["/etc"]),
                              ("cd nothing", ["sh: cd: nothing: not a directory"]),
                              ("cd /etc/issue", ["sh: cd: /etc/issue: not a directory"]), ("pwd", ["/etc"]),
                              ("cd ../tmp", []), ("mkdir sub", []), ("cd sub", []), ("pwd", ["/tmp/sub"]),
                              ("echo rel > note", []), ("cat /tmp/sub/note", ["rel"]), ("cat ../sub/./note", ["rel"]),
                              ("cd ../../..", []), ("pwd", ["/"]), ("cd /bin", []), ("./echo dot", ["dot"]),
                              ("../bin/echo up", ["up"]), ("cd", []), ("pwd", ["/"]),
                              ("rm tmp/sub/note", []), ("rmdir tmp/sub", [])]:
        out = run_command(m, command)
        check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    run_command(m, "cd /etc")
    out = run_command(m, "ls")
    check(names(out) == [".", "..", "group", "issue", "passwd", "shadow"], f"ls 不带参数应列出当前目录 /etc：{out!r}")
    # 根目录有 10 项，加上命令和提示符就比窗口高了：比 wc 的结果和其中一项
    check(run_command(m, "ls .. | wc") == run_command(m, "ls / | wc") and run_command(m, "ls .. | grep hello") == run_command(m, "ls / | grep hello"),
          "ls .. 应与 ls / 相同")
    wait_prompt(m)
    type_text(m, "exit\n")
    m.wait_for(lambda mm: "shell on hda: try help, ls /bin, cat /hello.txt, ps" in console(mm)[-2:], timeout=5,
               what="shell 退出后 init 再拉起一个新的 shell")
    check(run_command(m, "pwd") == ["/"], "新的 shell 是 init 的子进程，当前目录应是 init 的 /，不是上一个 shell 的 /etc")



def test_signals(m, elf):
    """信号：Ctrl+C 结束前台的程序和整条管道，shell 自己和后台程序不受影响；kill 结束后台程序；kill 的错误；shell 忽略 SIGTERM"""
    def ctrl_c_ends(command, what):
        wait_prompt(m)
        type_text(m, command + "\n")
        m.wait_for(lambda mm: last_line(mm) == f"$ {command}" or console(mm)[-1] == "" and f"$ {command}" in console(mm),
                   timeout=2, what=f"{command} 开始运行")
        time.sleep(0.5)
        m.sendkeys(["ctrl-c"])
        m.wait_for(lambda mm: [r for r in console(mm) if r][-3:] == [f"$ {command}", "^C", "$"], timeout=2,
                   what=f"按 Ctrl+C 后{what}结束，回显 ^C，shell 打出提示符")
        check(re.fullmatch(r"task \d+ killed by signal 2 \(SIGINT\)", message_row(m).strip()),
              f"第 7 行应写出被 SIGINT 结束：{message_row(m)!r}")

    ctrl_c_ends("letter G", "前台的 letter")      # test_shell 在后台留了一个写 D 的，这里用别的字母
    time.sleep(1.0)                 # 第 6 行每秒写 100 多个字母，1 秒后旧的 G 都被盖掉了
    check("G" not in task_output(m), f"letter G 结束后第 6 行不应再出现 G：{task_output(m)!r}")
    ctrl_c_ends("cat", "在终端上等输入的 cat")
    ctrl_c_ends("cat | wc", "管道的两段")
    names = [r.split()[-1] for r in run_command(m, "ps")[1:]]
    check(names.count("sh") == 1 and "cat" not in names and "wc" not in names, f"shell 还在，cat 和 wc 都结束了：{names!r}")

    out = run_command(m, "letter E &")
    check(len(out) == 1 and re.fullmatch(r"\[\d+\]", out[0]), f"后台运行应打印任务号：{out!r}")
    pid = out[0][1:-1]
    wait_prompt(m)
    type_text(m, "ech")
    m.sendkeys(["ctrl-c"])
    m.wait_for(lambda mm: [r for r in console(mm) if r][-2:] == ["$ ech^C", "$"], timeout=2,
               what="在提示符上按 Ctrl+C：回显 ^C，丢掉这一行；shell 的处理函数打断 read，重新打出提示符")
    check(run_command(m, "echo ok") == ["ok"], "丢掉的那一行不会交给 shell，接着敲的命令照常运行")
    time.sleep(1.0)
    check("E" in task_output(m), f"后台的 letter 忽略 SIGINT，按了 Ctrl+C 还在写 E：{task_output(m)!r}")
    check(run_command(m, "kill " + pid) == [], "kill 应成功")
    check(message_row(m).strip() == f"task {pid} killed by signal 15 (SIGTERM)", f"第 7 行应写出被 SIGTERM 结束：{message_row(m)!r}")
    time.sleep(1.0)
    check("E" not in task_output(m), f"后台的 letter 被 kill 后第 6 行不应再出现 E：{task_output(m)!r}")

    for command, expected in [("kill 1", ["kill: 1: cannot signal"]), ("kill 2", ["kill: 2: cannot signal"]),
                              ("kill 99", ["kill: 99: cannot signal"]), ("kill -x 5", ["kill: -x: bad signal"]),
                              ("kill", ["usage: kill [-signal] pid..."])]:
        out = run_command(m, command)
        check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    pid = run_command(m, "letter F &")[0][1:-1]
    check(run_command(m, f"kill -9 {pid}") == [] and message_row(m).strip() == f"task {pid} killed by signal 9 (SIGKILL)",
          f"kill -9 应发 SIGKILL：{message_row(m)!r}")
    check(run_command(m, "kill 4") == [], "shell 忽略 SIGTERM：kill 它自己返回 0，什么也不打印，shell 也不会重新拉起")
    out = run_command(m, "ps")
    check(any(r.split()[-1] == "sh" and r.split()[0] == "4" for r in out[1:]), f"按了几次 Ctrl+C、kill 了一次，shell 还是原来的 4 号任务：{out!r}")



def test_sort(m, elf):
    """sort：malloc 来的堆放下 600 KB 到 1.8 MB 的输入再排序；两个文件之间补换行；-r 倒序；结束后堆的页都收回"""
    free_before = m.screen(4)[3].split("/")[0]
    check(run_command(m, "cat /hello.txt /etc/issue | sort") == ["Hello from the Minix file system on hda!", "Linux 0.11 learning kernel"],
          "两行按字节排好")
    lines = run_command(m, "ls /etc")              # sort 比较整行：类型、大小、i 节点号、名字（/bin 和 / 太长，窗口放不下）
    out = run_command(m, "ls /etc | sort -r")
    check(out == sorted(lines, reverse=True), f"sort -r 把整行按字节倒着排：{out!r}")
    check(run_command(m, "sort /nothing") == ["sort: /nothing: cannot open"], "打不开的文件要报错")
    wait_prompt(m)
    type_text(m, "cat /big | sort | wc\n")
    m.wait_for(lambda mm: last_line(mm) == "$" and "$ cat /big | sort | wc" in console(mm), timeout=20, what="sort 排完 600 KB 的 /big")
    out = [r for r in console(m) if r][-2]
    check(out == "2401 7201 614401", f"排序不增减字节，最后一行补上换行：wc 应为 2401 7201 614401，实际是 {out!r}")
    for command, expected in [("cat /big /big /big | sort | wc", "7201 21601 1843201"), ("sort /big /big | wc", "4802 14402 1228802")]:
        wait_prompt(m)
        type_text(m, command + "\n")
        m.wait_for(lambda mm: last_line(mm) == "$" and f"$ {command}" in console(mm), timeout=30, what=f"{command} 排完")
        out = [r for r in console(m) if r][-2]
        check(out == expected, f"{command}：1.2～1.8 MB 的输入，realloc 在堆末尾原地变大才放得下；wc 应为 {expected}，实际是 {out!r}")
    m.wait_for(lambda mm: mm.screen(4)[3].split("/")[0] == free_before, timeout=2, what=f"sort 结束后空闲页回到 {free_before}")



def test_sleep(m, elf):
    """sleep：alarm 加 pause 睡上几秒；Ctrl+C 结束它；参数不对报用法"""
    for command, expected in [("sleep", ["usage: sleep seconds"]), ("sleep x", ["usage: sleep seconds"]), ("sleep 0", [])]:
        out = run_command(m, command)
        check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    wait_prompt(m)
    start = time.monotonic()
    type_text(m, "sleep 2\n")
    m.wait_for(lambda mm: last_line(mm) == "$" and "$ sleep 2" in console(mm), timeout=6, what="sleep 2 睡完")
    took = time.monotonic() - start
    check(1.5 <= took <= 4.5, f"sleep 2 应睡大约 2 秒，实际 {took:.1f} 秒")
    check(message_row(m).strip().endswith("exited with code 0"), f"sleep 正常退出：{message_row(m)!r}")
    wait_prompt(m)
    type_text(m, "sleep 100\n")
    time.sleep(0.5)
    m.sendkeys(["ctrl-c"])
    m.wait_for(lambda mm: [r for r in console(mm) if r][-3:] == ["$ sleep 100", "^C", "$"], timeout=2, what="Ctrl+C 结束 sleep 100")
    check(re.fullmatch(r"task \d+ killed by signal 2 \(SIGINT\)", message_row(m).strip()), f"sleep 被 SIGINT 结束：{message_row(m)!r}")



def test_cache(m, elf):
    """缓冲区缓存：写文件后 df 显示脏块，sync 之前一块也不写盘；不 sync，init 每隔大约 5 秒也写回，写回的正好是那些块"""
    def df():
        out = run_command(m, "df")[-1]
        g = re.fullmatch(r"cache: (\d+) of 16 buffers dirty; (\d+) blocks written since boot", out)
        check(g, f"df 的第二行：{out!r}")
        return int(g.group(1)), int(g.group(2))

    # init 每隔大约 5 秒 sync 一次：要是恰好落在写文件和 df 之间（甚至落在 echo 打开文件和写进去之间），
    # 看到的就是写回了一部分。那就删掉、换个名字再来，最多三次
    for attempt in range(3):
        name = f"/tmp/c{attempt}"
        check(run_command(m, "sync") == [], "sync 不打印东西")
        dirty, written = df()
        check(dirty == 0, f"sync 之后没有脏块：{dirty}")
        check(run_command(m, f"echo hi > {name}") == [], "写文件")
        dirty, now = df()
        if now == written:
            break
        check(run_command(m, f"rm {name}") == [], "删掉，再来一次")
    else:
        raise TestFailure("三次都碰上 init 的 sync")
    # i 节点位图、区块位图、新文件的 i 节点所在的那块 i 节点表、目录的数据块、文件的数据块；
    # 目录自己的 i 节点（修改时间变了）在另一块 i 节点表里的话，再多一块
    check(dirty in (5, 6), f"新建一个文件弄脏 5 块（目录的 i 节点在另一块 i 节点表里时 6 块），sync 之前一块也不写盘：{dirty}")
    m.wait_for(lambda mm: df()[0] == 0, timeout=12, what="init 每隔大约 5 秒 sync 一次，脏块写回")
    check(df()[1] == written + dirty, f"写回了正好 {dirty} 块：之前 {written}，现在 {df()[1]}")
    check(run_command(m, f"rm {name}") == [] and run_command(m, f"cat {name}") == [f"cat: {name}: cannot open"], "删掉")


def test_ln_mv(m, elf):
    """ln 和 mv：两个名字同一个 i 节点，删一个另一个还在；mv 改名、搬进目录、覆盖；目录搬不了"""
    ino = lambda out, name: next((l.split()[2] for l in out if l.split()[-1] == name), None)
    for command, expected in [("echo hi > /tmp/x", []), ("ln /tmp/x /tmp/y", [])]:
        check(run_command(m, command) == expected, f"{command} 应输出 {expected!r}")
    out = run_command(m, "ls /tmp")
    check(ino(out, "x") and ino(out, "x") == ino(out, "y"), f"x 和 y 是同一个 i 节点：{out!r}")
    first = ino(out, "x")
    for command, expected in [("rm /tmp/x", []), ("cat /tmp/y", ["hi"]), ("mv /tmp/y /tmp/z", []), ("mkdir /tmp/dd", []),
                              ("mv /tmp/z /tmp/dd", []), ("cat /tmp/dd/z", ["hi"]), ("echo a > /tmp/w", []),
                              ("mv /tmp/w /tmp/dd/z", []), ("cat /tmp/dd/z", ["a"]), ("mv /tmp/dd/z /tmp/dd/z", []),
                              ("cat /tmp/dd/z", ["a"]),
                              # 路径写法不同、其实是同一个文件：比的是 i 节点号，不是字符串，不然会先删掉唯一的名字
                              ("mv /tmp/dd/z /tmp/dd/./z", []), ("cat /tmp/dd/z", ["a"]), ("cd /tmp/dd", []),
                              ("mv z /tmp/dd/z", []), ("cat z", ["a"]), ("cd /", []),
                              ("ln /tmp/dd/z /tmp/zz/", ["ln: /tmp/zz/: cannot link"]), ("mv /tmp/dd/z /tmp/zz/", ["mv: /tmp/zz/: cannot move"]),
                              ("cat /tmp/dd/z", ["a"]), ("mv /tmp/dd /tmp/ee", ["mv: /tmp/dd: cannot move a directory"]),
                              ("ln /tmp/nothing /tmp/q", ["ln: /tmp/q: cannot link"]), ("ln /tmp /tmp/t2", ["ln: /tmp/t2: cannot link"]),
                              ("mv /tmp/nothing /tmp/q", ["mv: /tmp/nothing: no such file"]), ("ln", ["usage: ln file newname"])]:
        out = run_command(m, command)
        check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    out = run_command(m, "ls /tmp")
    check([l.split()[-1] for l in out] == [".", "..", "dd"], f"改名、搬走以后 /tmp 里只剩 dd：{out!r}")
    check(run_command(m, "rm /tmp/dd/z") == [] and run_command(m, "rmdir /tmp/dd") == [], "清理")
    check(first is not None, "有 i 节点号")



def test_script(m, elf):
    """脚本：sh 脚本一行行执行、跳过注释、cd 不影响外面；chmod 之后 #! 脚本能直接运行；#! 的参数排法和上限；嵌套、没有解释器、没有执行权限都不行；Ctrl+C 结束脚本"""
    lines = ["#!/bin/sh", "# a comment", "echo one", "cd /etc", "cat issue", "pwd"]
    check(run_command(m, f"echo {lines[0]} > /tmp/s") == [], "写脚本的第一行")
    for line in lines[1:]:
        check(run_command(m, f"echo {line} >> /tmp/s") == [], f"写脚本：{line}")
    want = ["one", "Linux 0.11 learning kernel", "/etc"]
    check(run_command(m, "sh /tmp/s") == want, "sh 脚本：一行行执行，# 开头的是注释")
    check(run_command(m, "pwd") == ["/"], "脚本里的 cd 换的是运行脚本的那个 sh 的当前目录，外面的 shell 不变")
    check(run_command(m, "/tmp/s") == ["sh: /tmp/s: not found"], "新建的文件权限是 0644，没有执行位，exec 不了")
    for command, expected in [("chmod 755 /tmp/s", []), ("/tmp/s", want), ("chmod", ["usage: chmod mode file..."]),
                              ("chmod 9 /tmp/s", ["usage: chmod mode file..."]), ("chmod 644 /nothing", ["chmod: /nothing: cannot change mode"]),
                              ("sh /nothing", ["sh: /nothing: cannot open"]),
                              ("echo #!/bin/hello one two > /tmp/h", []), ("chmod 755 /tmp/h", []),
                              ("/tmp/h a b", ["hello one two /tmp/h a b"]),
                              ("/tmp/h 1 2 3 4 5 6 7 8 9 10 11 12 13", ["hello one two /tmp/h 1 2 3 4 5 6 7 8 9 10 11 12 13"]),
                              ("/tmp/h 1 2 3 4 5 6 7 8 9 10 11 12 13 14", ["sh: /tmp/h: not found"]),
                              ("echo #! > /tmp/e", []), ("chmod 755 /tmp/e", []), ("/tmp/e", ["sh: /tmp/e: not found"]),
                              ("echo #!/tmp/h > /tmp/n", []), ("chmod 755 /tmp/n", []), ("/tmp/n", ["sh: /tmp/n: not found"])]:
        out = run_command(m, command)
        check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    check(run_command(m, "echo #!/bin/sh > /tmp/slow") == [] and run_command(m, "echo sleep 100 >> /tmp/slow") == [] and
          run_command(m, "echo echo after >> /tmp/slow") == [] and run_command(m, "chmod 755 /tmp/slow") == [], "写一个睡很久的脚本")
    out = run_command(m, "sh /tmp/slow &")
    check(len(out) == 1 and re.fullmatch(r"\[\d+\]", out[0]), f"后台运行脚本：{out!r}")
    pid = out[0][1:-1]
    # ps 的输出可能比窗口还长（前面的测试留下了后台程序），用 grep 只取要看的几行
    rows = [r.split() for r in run_command(m, "ps | grep s")]
    sleeper = [r[0] for r in rows if r[-1] == "sleep" and r[1] == pid]
    check(any(r[0] == pid and r[-1] == "sh" for r in rows) and len(sleeper) == 1, f"后台的 sh 在运行脚本，脚本里的 sleep 是它的子进程：{rows!r}")
    check(run_command(m, f"kill -9 {pid} {sleeper[0]}") == [], "结束后台的脚本和它的 sleep")
    wait_prompt(m)
    type_text(m, "/tmp/slow\n")
    time.sleep(0.5)
    m.sendkeys(["ctrl-c"])
    m.wait_for(lambda mm: [r for r in console(mm) if r][-3:] == ["$ /tmp/slow", "^C", "$"], timeout=2,
               what="按 Ctrl+C：运行脚本的 sh 和它里面的 sleep 都被结束（不会接着打印 after），回到提示符")
    out = run_command(m, "ps | grep s")
    check(not any(r.split()[-1] in ("slow", "sleep") for r in out), f"前台的脚本（ps 里叫 slow）和它的 sleep 都结束了：{out!r}")
    for f in ("/tmp/s", "/tmp/h", "/tmp/e", "/tmp/n", "/tmp/slow"):
        run_command(m, f"rm {f}")


def test_shell_input(m, elf):
    """shell 读命令：Ctrl+C 结束不了后台脚本里的命令；半行之后按 Ctrl+D 留着接上后面的字；半行之后按 Ctrl+C 丢掉它，shell 照常"""
    for line in ("echo sleep 2 > /tmp/bg", "echo echo after >> /tmp/bg"):
        check(run_command(m, line) == [], f"写脚本：{line}")
    out = run_command(m, "sh /tmp/bg &")
    check(len(out) == 1 and re.fullmatch(r"\[\d+\]", out[0]), f"后台运行脚本：{out!r}")
    start = time.time()
    time.sleep(0.3)
    m.sendkeys(["ctrl-c"])
    m.wait_for(lambda mm: "$ ^C" in console(mm), timeout=2, what="在提示符上按 Ctrl+C，终端回显 ^C")
    time.sleep(max(0, 1.3 - (time.time() - start)))
    printed = lambda mm: any("after" in r and "echo" not in r for r in console(mm))    # 不算敲进去的 echo echo after
    check(not printed(m), f"后台脚本里的 sleep 2 不该被 Ctrl+C 结束，1.3 秒时还不该打印 after：{console(m)!r}")
    m.wait_for(printed, timeout=4, what="后台脚本睡完 2 秒，接着打印 after")
    m.sendkeys(["ret"])
    wait_prompt(m)

    before = run_command(m, "ps | grep sh")
    type_text(m, "echo hi")
    m.sendkeys(["ctrl-d"])
    time.sleep(0.3)
    type_text(m, "x\n")
    m.wait_for(lambda mm: [r for r in console(mm) if r][-2:] == ["hix", "$"], timeout=3,
               what="敲 echo hi、Ctrl+D、再敲 x 回车：半行留着，接上 x，一起执行，打印 hix")

    type_text(m, "echo hi")
    m.sendkeys(["ctrl-d"])
    time.sleep(0.3)
    m.sendkeys(["ctrl-c"])
    m.wait_for(lambda mm: [r for r in console(mm) if r][-2:] == ["$ echo hi^C", "$"], timeout=3,
               what="敲 echo hi、Ctrl+D、再按 Ctrl+C：回到提示符")
    out = run_command(m, "echo ok")
    check(out == ["ok"], f"Ctrl+C 丢掉了那半行，接着敲的命令照常执行：{out!r}")
    rows = [r for r in console(m) if r]
    check("hi" not in rows, f"被 Ctrl+C 取消的半行 echo hi 不该执行：{rows!r}")
    after = run_command(m, "ps | grep sh")
    shells = lambda rows: [r.split()[0] for r in rows if r.split()[1:2] == ["2"] and r.endswith(" sh")]   # init 拉起的 shell
    check(len(shells(before)) == 1 and shells(before) == shells(after),
          f"还是原来那个 shell，没有退出、被 init 重新拉起：之前 {before!r}，之后 {after!r}")
    run_command(m, "rm /tmp/bg")


def test_script_edge_cases(m, elf):
    """脚本的边角：echo -n/-e；Tab 和行尾回车；超长的行报错跳过；目录和选项；exit 码；脚本的描述符不传下去；数字溢出；太长的命令名；#! 的行尾空白、相对路径的解释器、太长的第一行"""
    def run_all(cases):
        for command, expected in cases:
            out = run_command(m, command) if len(command) + 2 <= 80 else run_long(m, command)
            check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")

    run_all([("echo -n ab > /tmp/x", []), ("echo cd >> /tmp/x", []), ("cat /tmp/x", ["abcd"]),
             ("echo -e a\\tb\\\\ > /tmp/x", []), ("wc /tmp/x", ["1 2 5"]),
             # Tab 隔开的参数、Tab 缩进、行尾的回车
             ("echo -e echo\\tone\\r > /tmp/t", []), ("echo -e \\techo\\ttwo >> /tmp/t", []), ("sh /tmp/t", ["one", "two"]),
             ("echo exit 3 > /tmp/x3", []), ("echo exit 300 > /tmp/x300", []), ("echo check fds > /tmp/f", []),
             ("sh /tmp", ["sh: /tmp: is a directory"]), ("sh -x /tmp/t", ["sh: -x: unknown option"]),
             ("sh /tmp/f", ["fds: 0 1 2"]), ("check fds", ["fds: 0 1 2"]),
             ("sleep 4294967296", ["usage: sleep seconds"]), ("chmod 10000 /tmp/t", ["usage: chmod mode file..."]),
             ("kill 4294967297", ["kill: 4294967297: cannot signal"]), ("kill -4294967298 1", ["kill: -4294967298: bad signal"])])
    check(run_command(m, "sh /tmp/x3") == [] and re.fullmatch(r"task \d+ exited with code 3", message_row(m).strip()),
          f"脚本里的 exit 3：运行脚本的 sh 以 3 退出：{message_row(m)!r}")
    out = run_command(m, "sh /tmp/x300")
    check(out == ["sh: exit: 300: bad number"] and message_row(m).endswith("exited with code 0"),
          f"exit 300 超出 0～255：报错，接着读完脚本，以 0 退出：{out!r} {message_row(m)!r}")

    # 超过 127 个字节的一行：分两次写进去，sh 报错跳过它，接着执行下一行
    run_all([("echo -n echo " + "a" * 100 + " > /tmp/l", []), ("echo " + "b" * 40 + " >> /tmp/l", []),
             ("echo echo ok >> /tmp/l", []), ("sh /tmp/l", ["sh: line too long", "ok"])])
    name = "x" * 123                    # 加上 /bin/ 正好 128 个字节，放不下
    out = run_long(m, name)
    check("".join(out) == f"sh: {name}: name too long", f"太长的命令名不截短：{out!r}")

    # #!：行尾的空白和回车去掉；解释器可以是相对路径（从当前目录找起）；第一行太长就运行不了
    run_all([("echo -e #!/bin/hello one \\r > /tmp/q", []), ("chmod 755 /tmp/q", []), ("/tmp/q x", ["hello one /tmp/q x"]),
             ("echo -e #!/bin/sh\\r > /tmp/c", []), ("echo -e echo crlf\\r >> /tmp/c", []), ("chmod 755 /tmp/c", []),
             ("/tmp/c", ["crlf"]),
             ("echo #!/bin/sh -x > /tmp/o", []), ("chmod 755 /tmp/o", []), ("/tmp/o", ["sh: -x: unknown option"]),
             ("echo #!bin/sh > /tmp/r", []), ("echo echo rel >> /tmp/r", []), ("chmod 755 /tmp/r", []),
             ("/tmp/r", ["rel"]), ("cd /tmp", []), ("/tmp/r", ["sh: /tmp/r: not found"]), ("cd /", []),
             ("echo -n #!/bin/echo " + "a" * 90 + " > /tmp/w", []), ("echo " + "b" * 40 + " >> /tmp/w", []),
             ("chmod 755 /tmp/w", []), ("/tmp/w", ["sh: /tmp/w: not found"])])
    for f in ("x", "t", "x3", "x300", "f", "l", "q", "c", "o", "r", "w"):
        run_command(m, f"rm /tmp/{f}")


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)


def test_clock(m, elf):
    """时间：第 4 行和 date 显示的时间与宿主机的 UTC 时间一致；ls -l 的格式；新写的文件修改时间是现在；touch、chmod"""
    clock = re.search(r"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) UTC", m.screen(5)[4])
    check(clock is not None, f"第 4 行右边应显示时间：{m.screen(5)[4]!r}")
    shown = datetime.datetime.strptime(clock.group(1), "%Y-%m-%d %H:%M:%S")
    check(abs((shown - utc_now()).total_seconds()) <= 5, f"第 4 行的时间 {shown} 应和宿主机的 UTC 时间 {utc_now()} 差不到 5 秒")
    out = run_command(m, "date")
    check(len(out) == 1 and out[0].endswith(" UTC"), f"date 的输出：{out!r}")
    shown = datetime.datetime.strptime(out[0][:-4], "%Y-%m-%d %H:%M:%S")
    check(abs((shown - utc_now()).total_seconds()) <= 5, f"date 的时间 {shown} 应和宿主机的 UTC 时间 {utc_now()} 差不到 5 秒")
    line = re.compile(r"([-dcb])[-r][-w][-xsS][-r][-w][-xsS][-r][-w][-xtT] +\d+ +0 +0 +\d+ (\d{4}-\d\d-\d\d \d\d:\d\d) (\S+)")
    for row in run_command(m, "ls -l /etc") + run_command(m, "ls -l /dev"):    # / 有 10 项，窗口放不下
        check(line.fullmatch(row), f"ls -l 的格式：类型和权限、链接数、主人、组、大小、修改时间、名字：{row!r}")
    check(run_command(m, "ls -l /bin | grep sh")[0].startswith("-rwxr-xr-x  1    0    0 "), "/bin 里的程序有执行权限")
    check(run_command(m, "ls -l /dev | grep tty0")[0].startswith("crw-rw-rw-  1"), "设备文件")
    check(run_command(m, "echo hi > /tmp/clock1") == [] and run_command(m, "touch /tmp/clock2") == [] and
          run_command(m, "chmod 640 /tmp/clock2") == [], "写文件、touch、chmod")
    rows = {r.split()[-1]: r for r in run_command(m, "ls -l /tmp")}
    for name, mode in [("clock1", "-rw-r--r--"), ("clock2", "-rw-r-----")]:
        row = rows.get(name, "")
        check(row.startswith(mode), f"{name} 的权限应是 {mode}：{row!r}")
        when = datetime.datetime.strptime(line.fullmatch(row).group(2), "%Y-%m-%d %H:%M")
        check(abs((when - utc_now()).total_seconds()) <= 120, f"{name} 的修改时间 {when} 应该就是现在 {utc_now()}")
    check(run_command(m, "touch /nothing/x") == ["touch: /nothing/x: cannot touch"], "touch 不了的报错")
    check(run_command(m, "rm /tmp/clock1") == [] and run_command(m, "rm /tmp/clock2") == [], "清理")



def test_stderr(m, elf):
    """标准错误：出错的消息不进管道；2>&1 把它接回管道；2> 和 2>> 写进文件"""
    for command, expected in [("nosuch | wc", ["sh: nosuch: not found", "0 0 0"]), ("cat /nothing | wc", ["cat: /nothing: cannot open", "0 0 0"]),
                              ("cat /nothing 2>&1 | wc", ["1 4 27"]), ("nosuch 2> /tmp/err", []), ("ls /nothing 2>> /tmp/err", []),
                              ("cat /tmp/err", ["sh: nosuch: not found", "ls: /nothing: not a directory"]),
                              ("echo out > /tmp/o 2>&1", []), ("cat /tmp/o", ["out"]), ("rm /tmp/err", []), ("rm /tmp/o", [])]:
        out = run_command(m, command)
        check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")


# 前几项共用一台正常启动的虚拟机；其余各自启动自检内核。
def test_nice_shell(m, elf):
    """nice、ps -l、time：nice 打印和调整 nice 值，后台的 nice -n 5 sleep 在 ps -l 里是 PRI 15、NI 5；time 报告过了多久、用了多少 CPU"""
    usage = ["usage: nice [-n increment] [command args...]"]
    for command, expected in [("nice", ["0"]), ("nice -n 3 nice", ["3"]), ("nice nice", ["10"]), ("nice -n -5 nice", ["-5"]),
                              ("nice -n 30 nice", ["19"]), ("nice -n -30 nice", ["-20"]), ("nice -n 41 nice", usage),
                              ("nice -n x nice", usage), ("nice -n", usage), ("nice nosuch", ["nice: nosuch: not found"]),
                              ("nice -n 4", ["4"])]:
        out = run_command(m, command)
        check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    out = run_command(m, "nice -n 5 sleep 3 &")
    check(len(out) == 1 and re.fullmatch(r"\[\d+\]", out[0]), f"后台运行：{out!r}")
    pid = out[0][1:-1]
    out = run_command(m, "ps -l | grep sleep")
    rows = [r.split() for r in out]
    check(any(r[0] == pid and r[2:6] == ["S", "0", "15", "5"] and r[-1] == "sleep" for r in rows),
          f"ps -l：root 运行的 nice 5 的 sleep 每轮 15 个 tick（UID 0、PRI 15、NI 5），在睡（S）：{out!r}")
    # ps -l 的输出比窗口还长，分几次 grep 出来看
    out = run_command(m, "ps -l | grep PID") + run_command(m, "ps -l | grep i") + run_command(m, "ps -l | grep sh")
    check(out[0] == "PID PPID S   UID PRI  NI    TIME NAME", f"ps -l 的表头：{out[0]!r}")
    for r in out[1:]:
        check(re.fullmatch(r" ?\d+ +\d+ [RSZWB] +\d+ +\d+ +-?\d+ +\d+\.\d\d \S+", r),
              f"ps -l 每行是任务号、父进程、状态、UID、PRI、NI、TIME、名字：{r!r}")
    idle = next(r.split() for r in out[1:] if r.split()[-1] == "idle")
    check(idle[3:7] == ["0", "20", "0", "0.00"], f"任务 0（空闲任务）是 root 的，它的时间单独记成空闲比例，不算进 TIME：{idle!r}")

    tm = re.compile(r"real (\d+\.\d\d) user (\d+\.\d\d) sys (\d+\.\d\d)")
    out = run_command(m, "time sleep 1")
    g = len(out) == 1 and tm.fullmatch(out[0])
    check(g and 0.95 <= float(g.group(1)) <= 1.2 and float(g.group(2)) + float(g.group(3)) <= 0.05,
          f"time sleep 1：过了 1 秒左右，CPU 几乎没用：{out!r}")
    out = run_command(m, "time check spin")
    g = len(out) == 1 and tm.fullmatch(out[0])
    real, user, sys_ = (float(g.group(i)) for i in (1, 2, 3)) if g else (0, 0, 0)
    check(g and 0.45 <= real <= 0.7 and user >= 0.4 and sys_ <= 0.05 and user + sys_ <= real + 0.02,
          f"time check spin：在用户态空转半秒，时间几乎都记在 user：{out!r}")
    out = run_command(m, "time sort /big > /tmp/sorted")
    g = len(out) == 1 and tm.fullmatch(out[0])
    real, user, sys_ = (float(g.group(i)) for i in (1, 2, 3)) if g else (0, 0, 0)
    check(g and user + sys_ <= real + 0.02, f"time sort /big：大半时间在等硬盘，用的 CPU 不超过过了的时间：{out!r}")
    out = run_command(m, "time")
    check(out == ["real 0.00 user 0.00 sys 0.00"], f"time 后面没有命令：{out!r}")
    check(run_command(m, "rm /tmp/sorted") == [], "删掉")
    m.wait_for(lambda mm: "sleep" not in " ".join(run_command(mm, "ps")), timeout=5, what="后台的 sleep 3 睡完退出")


def test_users(m, elf):
    """用户：id；/tmp 有粘滞位、/home/user 是 user 的；chown 用名字或号码改主人和组，错误情况；ls -l 显示 setuid、setgid、粘滞位"""
    entry = lambda path, name: next((r for r in run_command(m, f"ls -l {path} | grep {name}") if r.split()[-1] == name), "")
    check(run_command(m, "id") == ["uid=0(root) gid=0(root)"], "shell 是 root 运行的")
    check(entry("/", "tmp").startswith("drwxrwxrwt "), f"/tmp 谁都能写，有粘滞位：{entry('/', 'tmp')!r}")
    check(entry("/home", "user").split()[:4] == ["drwxr-xr-x", "2", "100", "100"], f"/home/user 是 user（100）的：{entry('/home', 'user')!r}")
    for command, expected in [("echo hi > /tmp/owned", []), ("chown user /tmp/owned", []), ("chown", ["usage: chown owner[:group] file..."]),
                              ("chown nosuch /tmp/owned", ["chown: nosuch: no such user"]),
                              ("chown user:nogroup /tmp/owned", ["chown: nogroup: no such group"]),
                              ("chown 1 /nothing", ["chown: /nothing: cannot change owner"])]:
        out = run_command(m, command)
        check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    check(entry("/tmp", "owned").split()[2:4] == ["100", "0"], f"chown user：主人换成 100，组不变：{entry('/tmp', 'owned')!r}")
    check(run_command(m, "chown user:users /tmp/owned") == [] and entry("/tmp", "owned").split()[2:4] == ["100", "100"], "chown user:users")
    check(run_command(m, "chown 0:0 /tmp/owned") == [] and entry("/tmp", "owned").split()[2:4] == ["0", "0"], "chown 0:0：写号码也行")
    for mode, shown in [("4755", "-rwsr-xr-x"), ("2755", "-rwxr-sr-x"), ("4644", "-rwSr--r--"), ("1777", "-rwxrwxrwt"), ("1776", "-rwxrwxrwT")]:
        check(run_command(m, f"chmod {mode} /tmp/owned") == [] and entry("/tmp", "owned").startswith(shown + " "),
              f"chmod {mode} 之后 ls -l 显示 {shown}：{entry('/tmp', 'owned')!r}")
    check(run_command(m, "rm /tmp/owned") == [], "删掉")


def test_login(m, elf):
    """su、login、passwd：/etc/shadow 只有 root 能读；su 问密码时不回显；换成 user 以后权限都按 user 算；passwd 改密码；login 进家目录"""
    banner = "shell on hda: try help, ls /bin, cat /hello.txt, ps"

    def answer(prompt, text):
        m.wait_for(lambda mm: last_line(mm) == prompt.rstrip(), timeout=5, what=f"提示 {prompt!r}")
        type_text(m, text + "\n")

    def new_shell(what):
        m.wait_for(lambda mm: [r for r in console(mm) if r][-2:] == [banner, "$"], timeout=5, what=what)

    def run_all(cases):
        for command, expected in cases:
            out = run_command(m, command)
            check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")

    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
    import mkshadow
    shadow = run_command(m, "cat /etc/shadow")
    check(shadow == [f"root:{mkshadow.hash_password('root', 'root')}", f"user:{mkshadow.hash_password('user', 'user')}"],
          f"/etc/shadow 里是 名字:散列，和 tools/mkshadow.py 算的一样：{shadow!r}")
    rows = run_command(m, "ls -l /etc") + run_command(m, "ls -l /bin | grep su") + run_command(m, "ls -l /bin | grep login")
    perms = {r.split()[-1]: r.split()[0] for r in rows + run_command(m, "ls -l /bin | grep passwd")}
    check(perms.get("shadow") == "-rw-------" and perms.get("su") == "-rwsr-xr-x" and perms.get("passwd") == "-rwsr-xr-x"
          and perms.get("login") == "-rwxr-xr-x", f"shadow 只有 root 能读写，su、passwd 是 setuid 程序：{perms!r}")
    out = run_command(m, "sleep 30 &")
    sleeper = out[0][1:-1]

    wait_prompt(m)
    type_text(m, "su user\n")
    new_shell("root 运行 su 不用输密码，换成 user 运行一个新的 shell")
    run_all([("id", ["uid=100(user) gid=100(users)"]), ("cat /etc/shadow", ["cat: /etc/shadow: cannot open"]),
             ("echo x > /x", ["sh: /x: cannot redirect"]), (f"kill {sleeper}", [f"kill: {sleeper}: cannot signal"]),
             ("nice -n -5 nice", ["nice: cannot set niceness", "0"]), ("cd /home/user", []), ("echo hi > f", [])])
    row = next((r for r in run_command(m, "ls -l") if r.split()[-1] == "f"), "")
    check(row.split()[2:4] == ["100", "100"], f"user 新建的文件是 user 的：{row!r}")
    run_all([("rm f", []), ("cd /", [])])

    type_text(m, "su user\n")
    m.wait_for(lambda mm: last_line(mm) == "password:", timeout=5, what="不是 root，su 要问密码")
    type_text(m, "us")
    time.sleep(0.3)
    check(last_line(m) == "password:", f"回显关着：敲的密码不显示在屏幕上：{last_line(m)!r}")
    type_text(m, "er\n")
    new_shell("密码对了")
    run_all([("id", ["uid=100(user) gid=100(users)"]), ("exit", [])])
    type_text(m, "su\n")
    answer("password: ", "user")
    m.wait_for(lambda mm: [r for r in console(mm) if r][-2:] == ["su: wrong password", "$"], timeout=5, what="root 的密码不对")
    type_text(m, "su\n")
    answer("password: ", "root")
    new_shell("user 运行 su，输对 root 的密码，变回 root")
    run_all([("id", ["uid=0(root) gid=0(root)"]), ("exit", []), ("id", ["uid=100(user) gid=100(users)"])])

    type_text(m, "passwd\n")
    answer("old password: ", "user")
    answer("new password: ", "secret")
    answer("retype new password: ", "secret")
    m.wait_for(lambda mm: [r for r in console(mm) if r][-2:] == ["password changed", "$"], timeout=5, what="passwd 改好了密码")
    run_all([("passwd root", ["passwd: you may only change your own password"])])
    type_text(m, "su user\n")
    answer("password: ", "user")
    m.wait_for(lambda mm: [r for r in console(mm) if r][-2:] == ["su: wrong password", "$"], timeout=5, what="旧密码不对了")
    type_text(m, "su user\n")
    answer("password: ", "secret")
    new_shell("新密码对")
    run_all([("exit", []), ("exit", []), ("id", ["uid=0(root) gid=0(root)"])])
    type_text(m, "passwd user\n")
    answer("new password: ", "user")                  # root 改别人的密码不用输原来的
    answer("retype new password: ", "user")
    m.wait_for(lambda mm: [r for r in console(mm) if r][-2:] == ["password changed", "$"], timeout=5, what="root 把密码改回去")
    check(run_command(m, "cat /etc/shadow") == shadow, "改回去以后 shadow 和原来一样")

    type_text(m, "login\n")
    answer("login: ", "user")
    answer("password: ", "wrong")
    m.wait_for(lambda mm: [r for r in console(mm) if r][-2:] == ["login incorrect", "login:"], timeout=5, what="密码不对，再问一次")
    type_text(m, "user\n")
    answer("password: ", "user")
    new_shell("login 成功，运行 user 的 shell")
    run_all([("pwd", ["/home/user"]), ("id", ["uid=100(user) gid=100(users)"]), ("exit", [])])
    type_text(m, "login\n")
    m.wait_for(lambda mm: last_line(mm) == "login:", timeout=5, what="login 问用户名")
    m.sendkeys(["ctrl-d"])
    wait_prompt(m)
    run_all([("su nosuch", ["su: nosuch: no such user"]), ("id", ["uid=0(root) gid=0(root)"]), (f"kill {sleeper}", [])])


def test_serial(m, elf):
    """串口终端：开机后串口上是 login；登录以后的 shell 和控制台的互不相干：各自的输出、各自的 Ctrl+C；回车、退格、回显；exit 后 init 再拉起 login"""
    def serial_run(command):
        m.serial_send(command + "\r")
        lines = m.serial_wait("$ ", what=f"{command!r} 运行完、shell 再打出提示符").split("\r\n")
        check(lines[0] == command and lines[-1] == "$ ", f"串口上的回显和提示符：{lines!r}")
        return lines[1:-1]

    m.serial_wait("login: ", timeout=10, what="串口上 init 拉起的 login")
    m.serial_send("user\r")
    out = m.serial_wait("password: ")
    check(out == "user\r\npassword: ", f"终端敲的回车是 \\r，当成换行；回显的换行是 \\r\\n：{out!r}")
    m.serial_send("user\r")
    out = m.serial_wait("$ ")
    check(out == "\r\nshell on hda: try help, ls /bin, cat /hello.txt, ps\r\n$ ", f"密码不回显，登录后运行 user 的 shell：{out!r}")
    for command, expected in [("id", ["uid=100(user) gid=100(users)"]), ("pwd", ["/home/user"]), ("echo only-serial", ["only-serial"])]:
        out = serial_run(command)
        check(out == expected, f"串口上 {command} 应输出 {expected!r}，实际是 {out!r}")
    check(not any("only-serial" in r for r in console(m)), f"串口上的输出不出现在控制台：{console(m)!r}")
    check(run_command(m, "echo only-console") == ["only-console"], "控制台照常")
    m.serial_read()
    check(b"only-console" not in m.serial_buf, "控制台的输出不出现在串口上")
    m.serial_send("ecx\x7fho hi\r")
    out = m.serial_wait("$ ")
    check(out == "ecx\b \bho hi\r\nhi\r\n$ ", f"终端的退格键送来 DEL：删掉 x，回显“退格、空格、退格”把它擦掉：{out!r}")

    m.serial_send("sleep 10\r")
    time.sleep(0.5)
    m.serial_send("\x03")
    out = m.serial_wait("$ ", timeout=2, what="串口上按 Ctrl+C 结束 sleep 10")
    check(out == "sleep 10\r\n^C\r\n$ ", f"串口上的 Ctrl+C 结束串口的前台程序：{out!r}")
    m.serial_send("sleep 2\r")
    start = time.time()
    m.serial_wait("sleep 2\r\n")
    wait_prompt(m)
    m.sendkeys(["ctrl-c"])
    m.serial_wait("$ ", timeout=4)
    check(time.time() - start >= 1.8, f"控制台上的 Ctrl+C 不影响串口上的 sleep 2：{time.time() - start:.1f} 秒就结束了")
    wait_prompt(m)
    rows = [r.split() for r in run_command(m, "ps -l | grep sh")]
    check(any(r[1] == "2" and r[3] == "100" and r[-1] == "sh" for r in rows), f"ps 里有 init 拉起、user 运行的那个 shell：{rows!r}")

    m.serial_send("su\r")
    m.serial_wait("password: ")
    m.serial_send("root\r")
    out = m.serial_wait("$ ")
    check("root" not in out, f"在串口上输密码也不回显：{out!r}")
    check(serial_run("id") == ["uid=0(root) gid=0(root)"] and serial_run("exit") == [], "su 变成 root，再 exit 回来")
    m.serial_send("exit\r")
    m.serial_wait("login: ", timeout=5, what="user 的 shell 退出以后，init 再拉起一个 login")
    m.serial_send("user\r")
    m.serial_wait("password: ")
    m.serial_send("bad\r")
    out = m.serial_wait("login: ")
    check(out == "\r\nlogin incorrect\r\nlogin: ", f"密码不对：{out!r}")


def test_mount(m, elf):
    """第二块盘：挂上 hdb 以后 /mnt 是它的根目录，/mnt/.. 回到 /；读写；df 每块盘一行；不能跨盘链接；开着文件卸不了；挂载点删不掉；卸了再挂内容还在；只有 root 能挂"""
    names = lambda out: [r.split()[-1] for r in out]
    for command, expected in [("mount", ["/dev/hda on /"]), ("ls /mnt", None), ("mount /dev/hdb /mnt", []),
                              ("mount", ["/dev/hda on /", "/dev/hdb on /mnt"]), ("cat /mnt/readme", ["This file is on the second disk (hdb)."]),
                              ("ls / | wc", None), ("cat /mnt/../hello.txt", ["Hello from the Minix file system on hda!"]),
                              ("echo hi > /mnt/new", []), ("cat /mnt/new", ["hi"]), ("ln /mnt/new /tmp/x", ["ln: /tmp/x: cannot link"]),
                              ("mv /mnt/new /tmp/x", ["mv: /tmp/x: cannot move"]),
                              ("mount /dev/hdb /mnt", ["mount: /dev/hdb: cannot mount"]), ("mount /dev/hdb /tmp", ["mount: /dev/hdb: cannot mount"]),
                              ("mount /dev/hda /tmp", ["mount: /dev/hda: cannot mount"]), ("mount /dev/tty0 /tmp", ["mount: /dev/tty0: cannot mount"]),
                              ("mount /hello.txt /tmp", ["mount: /hello.txt: cannot mount"]), ("mount /dev/hdb /hello.txt", ["mount: /dev/hdb: cannot mount"]),
                              ("mount /dev/hdb", ["usage: mount [device dir]"]), ("umount /", ["umount: /: cannot unmount"]),
                              ("umount /dev/hda", ["umount: /dev/hda: cannot unmount"]), ("umount /tmp", ["umount: /tmp: cannot unmount"]),
                              ("rmdir /mnt", ["rmdir: /mnt: cannot remove (not an empty directory?)"]), ("cat /dev/hdb", ["cat: /dev/hdb: cannot open"])]:
        out = run_command(m, command)
        if command == "ls /mnt":
            check(names(out) == [".", ".."], f"挂上之前 /mnt 是一个空目录：{out!r}")
        elif command == "ls / | wc":
            root = out
        else:
            check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    check(run_command(m, "ls /mnt/.. | wc") == root, "/mnt/.. 是 / ：在挂上来的盘的根目录里查 ..，先回到挂载点")
    rows = {r.split()[-1]: r.split() for r in run_command(m, "ls /mnt")}
    check(rows["."][2] == "1" and rows["readme"][2] == "2" and rows[".."][2] == "1", f"/mnt 是 hdb 的根目录（1 号 i 节点），i 节点号是 hdb 上的：{rows!r}")
    df = run_command(m, "df")
    check(len(df) == 3 and df[0].startswith("/: ") and re.fullmatch(r"/mnt: blocks: \d+ used, \d+ free; inodes: 3 used, 125 free", df[1]),
          f"df 每块盘一行，hdb 有 128 个 i 节点，用了 3 个（根目录、readme、new）：{df!r}")
    check(entry_row(m, "/dev", "hdb").startswith("brw------- "), "/dev/hdb 是块设备，只有 root 能用")

    out = run_command(m, "sleep 30 < /mnt/new &")
    sleeper = out[0][1:-1]
    check(run_command(m, "umount /mnt") == ["umount: /mnt: cannot unmount"], "/mnt/new 还开着：卸不了")
    check(run_command(m, f"kill {sleeper}") == [], "结束开着它的 sleep")
    m.wait_for(lambda mm: run_command(mm, "umount /mnt") == [], timeout=5, what="sleep 结束以后就能卸了")
    for command, expected in [("mount", ["/dev/hda on /"]), ("cat /mnt/new", ["cat: /mnt/new: cannot open"]), ("umount /mnt", ["umount: /mnt: cannot unmount"]),
                              ("mount /dev/hdb /mnt", []), ("cat /mnt/new", ["hi"]), ("rm /mnt/new", []), ("umount /dev/hdb", []),
                              ("su user", None), ("mount /dev/hdb /mnt", ["mount: /dev/hdb: cannot mount"]), ("exit", [])]:
        if expected is None:
            wait_prompt(m)
            type_text(m, command + "\n")
            m.wait_for(lambda mm: [r for r in console(mm) if r][-1:] == ["$"] and "shell on hda: try help, ls /bin, cat /hello.txt, ps" in console(mm)[-2:],
                       timeout=5, what="换成 user 的 shell")
            continue
        out = run_command(m, command)
        check(out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    check(run_command(m, "mount") == ["/dev/hda on /"], "最后卸掉了")


def entry_row(m, path, name):
    return next((r for r in run_command(m, f"ls -l {path} | grep {name}") if r.split()[-1] == name), "")


def test_demand_loading(m, elf):
    """按需装入：程序的页用到才从硬盘读；同一个程序同时运行两份，只读的页共用；运行中的程序删掉了照样运行；运行中的程序写不了，开着写的文件也运行不了"""
    def memory():
        g = re.fullmatch(r"memory: (\d+)/3840 pages free  page faults: \d+  copy-on-write: \d+  shared: (\d+)", m.screen(4)[3])
        check(g, f"第 3 行：{m.screen(4)[3]!r}")
        return int(g.group(1)), int(g.group(2))

    def settle(command):
        """在后台运行 command，等它读完表、睡下，返回这时的空闲页数和共用次数"""
        check(re.fullmatch(r"\[\d+\]", run_command(m, command + " &")[0]), f"后台运行 {command}")
        time.sleep(1)
        return memory()

    check(run_command(m, "check lazy > /tmp/lazy0") == [], "先在前台运行一次：shell 自己的页（写时复制的栈）都备好了")
    base, shared0 = memory()
    free1, _ = settle("check lazy > /tmp/lazy1")
    used1 = base - free1
    check(used1 <= 10, f"只读表的第一页：一共只多用了 {used1} 页（内核栈、页目录、页表、栈、用到的代码和数据，加表的一页）")
    time.sleep(3)
    free2, _ = settle("check lazy all > /tmp/lazy2")
    used2 = base - free2
    check(used2 - used1 >= 15, f"读遍 16 页的表：比只读一页多用了 {used2 - used1} 页，应至少多 15 页（第一次 {used1}，这次 {used2}）")
    free3, shared3 = settle("check lazy all > /tmp/lazy3")
    check(free2 - free3 <= used1 and shared3 - shared0 >= 16,
          f"同时再运行一份：表的 16 页和代码页都和前一份共用，只多用了 {free2 - free3} 页，共用了 {shared3 - shared0} 次")
    time.sleep(3)
    check(run_command(m, "cat /tmp/lazy1 /tmp/lazy2 /tmp/lazy3") == ["lazy ok"] * 3, "每一份读到的表都对")
    run_command(m, "echo")              # shell 读命令之前收走结束了的后台程序
    check(memory()[0] == base, f"都结束以后空闲页回到 {base}：{memory()[0]}")

    inodes = lambda: run_command(m, "df")[0].split("inodes: ")[1]
    check(run_command(m, "echo > /tmp/lazy4") == [], "先建好输出文件，免得它多占一个 i 节点")
    before = inodes()
    for command, expected in [("cat /bin/check > /tmp/lz", []), ("chmod 755 /tmp/lz", []), ("/tmp/lz lazy late > /tmp/lazy4 &", None),
                              ("rm /tmp/lz", []), ("cat /tmp/lz", ["cat: /tmp/lz: cannot open"])]:
        out = run_command(m, command)
        check(expected is None or out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    check(inodes() != before, f"名字删了，可程序还在运行：i 节点还没收回：{inodes()!r}")
    time.sleep(2)
    check(run_command(m, "cat /tmp/lazy4") == ["lazy ok"], "程序文件删掉了，它还在运行，后面用到的页照样从删掉的文件里读出来")
    run_command(m, "echo")
    check(inodes() == before, f"程序结束以后，删掉的文件的 i 节点和块才收回：{inodes()!r}，原来 {before!r}")

    for command, expected in [("cat /bin/check > /tmp/lz", []), ("chmod 755 /tmp/lz", []), ("/tmp/lz lazy > /tmp/lazy5 &", None),
                              ("echo x >> /tmp/lz", ["sh: /tmp/lz: cannot redirect"])]:
        out = run_command(m, command)
        check(expected is None or out == expected, f"{command} 应输出 {expected!r}，实际是 {out!r}")
    time.sleep(3.5)
    check(run_command(m, "cat /tmp/lazy5") == ["lazy ok"], "程序在运行，它的文件写不进去，程序没被改坏")
    out = run_command(m, "/tmp/lz lazy >> /tmp/lz")
    check(out == ["sh: /tmp/lz: not found"], f"子进程先开着 /tmp/lz 写，再 exec 它：运行不了：{out!r}")
    check(run_command(m, "/tmp/lz lazy late") == ["lazy ok"], "文件没被改，照样能运行")
    for f in ("lz", "lazy0", "lazy1", "lazy2", "lazy3", "lazy4", "lazy5"):
        run_command(m, f"rm /tmp/{f}")


def test_raw_tty(m, elf):
    """终端的原始模式和方向键：关掉 ICANON 以后一个字节一个字节交给程序，方向键是 ESC [ A；规范模式丢掉方向键；控制台认得 ESC [ n D 和 ESC [ K"""
    rows = lambda: [r for r in console(m) if r]
    wait_prompt(m)
    type_text(m, "check raw\n")
    m.wait_for(lambda mm: rows()[-1:] == ["$ check raw"], timeout=3, what="check raw 开始读")
    type_text(m, "a")
    m.sendkeys(["up"])
    m.wait_for(lambda mm: rows()[-2:] == ["97 27 91 65", "$"], timeout=3,
               what="不用敲回车就读到 a 和上箭头的三个字节 ESC [ A，回显关着（屏幕上没有 a）")
    wait_prompt(m)
    type_text(m, "cat\n")
    m.wait_for(lambda mm: rows()[-1:] == ["$ cat"], timeout=3, what="cat 开始读")
    type_text(m, "x")
    m.sendkeys(["left", "right", "up", "down"])
    type_text(m, "y\n")
    m.wait_for(lambda mm: rows()[-2:] == ["xy", "xy"], timeout=3, what="规范模式把方向键的转义序列整个丢掉：这一行是 xy")
    m.sendkeys(["ctrl-d"])
    wait_prompt(m)
    for command, expected in [("echo -e abc\\e[2Dx", ["axc"]), ("echo -e abcdef\\e[3D\\e[K", ["abc"]), ("echo -e ab\\e[Zcd", ["abcd"])]:
        out = run_command(m, command)
        check(out == expected, f"控制台的转义序列：{command} 应显示 {expected!r}，实际是 {out!r}")
    m.serial_send("user\r")              # 串口那头停在 login: 上，先登录
    m.serial_wait("password: ")
    m.serial_send("user\r")
    m.serial_wait("$ ")
    m.serial_send("cat\r")
    m.serial_wait("cat\r\n")
    m.serial_send("p\x1b[Dq\r")
    m.serial_wait("pq\r\n" * 2, what="串口上的规范模式也丢掉方向键：回显和 cat 打印的都是 pq")
    m.serial_send("\x04")
    m.serial_wait("$ ")
    m.serial_send("exit\r")
    m.serial_wait("login: ")


SHARED_TESTS = [test_boot_screen, test_kernel_in_memory, test_protected_mode, test_paging,
                test_timer_rate, test_keyboard_echo, test_keyboard_shift, test_console_scroll,
                test_idle_in_hlt, test_demand_paging, test_fork_address_spaces, test_syscall_gate, test_user_setup, test_user_task,
                test_tasks_alternate, test_shell, test_cwd, test_signals, test_sort, test_sleep, test_cache, test_ln_mv, test_script, test_shell_input, test_script_edge_cases, test_clock, test_stderr, test_nice_shell, test_users, test_login, test_serial, test_mount, test_demand_loading, test_raw_tty]
SEPARATE_TESTS = [test_syscalls, test_page_alloc, test_double_free, test_hd_rw, test_early_key, test_keyboard_edge_cases, test_divide_error, test_bad_gate, test_register_dump, test_unmapped_read, test_null_write,
                  test_clobber_loader, test_other_segments,
                  test_big_kernel, test_user_write_kernel, test_user_privileged,
                  test_user_bad_pointer, test_user_exit, test_all_tasks_exit,
                  test_nmi_in_user_mode, test_user_write_code, test_preemption,
                  test_deep_stack, test_wild_read, test_fork_isolation, test_waitpid, test_file_system, test_exec, test_pipe, test_fs_write, test_signal_syscalls, test_brk, test_alarm, test_sync, test_sync_to_disk, test_link, test_time_syscalls, test_fcntl, test_nice, test_permissions, test_no_disk]


def main():
    image = os.path.join(BUILD, "floppy.img")
    elf = os.path.join(BUILD, "kernel.elf")
    for path in (image, elf):
        if not os.path.exists(path):
            sys.exit(f"找不到 {path}，请先运行 make")

    failures = 0

    def run(test, m):
        nonlocal failures
        start = time.monotonic()
        try:
            test(m, elf)
            status = "PASS"
            detail = ""
        except TestFailure as e:
            failures += 1
            status, detail = "FAIL", f"\n       {e}"
        except Exception as e:      # 测试脚本自己的意外错误：只算这一项失败，其余照常运行
            failures += 1
            status, detail = "FAIL", f"\n       意外的 {type(e).__name__}：{e}"
        print(f"{status}  {test.__doc__}（{time.monotonic() - start:.1f}s）{detail}", flush=True)

    # 正常内核没能启动时，依赖它的各项都记为失败，但自检内核的各项照常运行
    boot_error = None
    try:
        with Machine(image) as m:
            try:
                m.wait_for(boot_ready, what="启动完成（第 0 行出现 ABPKCVITU）")
            except TestFailure as e:
                boot_error = e
            else:
                for test in SHARED_TESTS:
                    run(test, m)
    except TestFailure as e:
        boot_error = e
    if boot_error:
        failures += len(SHARED_TESTS)
        print(f"FAIL  正常内核没有启动完成，依赖它的 {len(SHARED_TESTS)} 项都记为失败：{boot_error}", flush=True)
    for test in SEPARATE_TESTS:
        run(test, None)

    total = len(SHARED_TESTS) + len(SEPARATE_TESTS)
    print(f"\n{total - failures}/{total} 项通过")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
