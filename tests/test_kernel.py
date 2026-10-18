#!/usr/bin/env python3
"""
make test：在无头 QEMU 中启动内核，检查启动输出、定时器、键盘和异常处理。

只用 Python 标准库。测试通过 QEMU 监视器（一个 unix socket）读取显存和寄存器、
模拟按键；用系统自带的 objdump 查符号地址和反汇编。
需要触发异常的测试，会用 make 的 SELFTEST 参数另外构建内核，放在 build/selftest/ 下。

在仓库根目录运行：python3 tests/test_kernel.py（make test 会先构建再调用它）。
"""
import os
import re
import socket
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

    def __init__(self, image):
        tmp = tempfile.mkdtemp(prefix="kernel-test-")
        self.sock_path = os.path.join(tmp, "monitor.sock")
        self.log_path = os.path.join(tmp, "qemu.log")
        self.sock = None
        # QEMU 的错误输出写进文件：用管道却不读，输出多了会把 QEMU 卡住
        # 同一目录下的 hd.img 接在主 IDE 通道上；snapshot=on 让写入只留在临时文件里，镜像本身不变
        hd = os.path.join(os.path.dirname(image), "hd.img")
        disk = ["-drive", f"file={hd},format=raw,if=ide,index=0,snapshot=on"] if os.path.exists(hd) else []
        with open(self.log_path, "w") as log:
            self.proc = subprocess.Popen(
                [QEMU, "-machine", "pc", "-accel", "tcg", "-m", "16M",
                 "-drive", f"file={image},format=raw,if=floppy,index=0", *disk, "-boot", "order=a",
                 "-display", "none", "-nic", "none", "-serial", "none",
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

    def close(self):
        try:
            if self.sock:
                self.sock.sendall(b"quit\n")
        except OSError:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


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
    # 写时复制的次数取决于 fork 之后谁先写栈，不卡死；用掉的页数是 A 的内核栈，init、letter、sh 各自的
    # 内核栈、页目录、页表、栈页，letter 的代码页，sh 的代码页和数据页，约 15 页
    match = re.fullmatch(r"memory: (\d+)/3840 pages free  page faults: 1  copy-on-write: (\d+)", rows[3])
    check(match and 3815 <= int(match.group(1)) <= 3830 and int(match.group(2)) >= 2, f"第 3 行不对：{rows[3]!r}")
    check(re.fullmatch(r"ticks: \d+( +idle: +\d+%)?", rows[4]) is not None, f"第 4 行不对：{rows[4]!r}")
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
             "!": "shift-1", "@": "shift-2", "|": "shift-backslash", "\n": "ret"}


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
    names = [".", "..", "cat", "check", "echo", "grep", "hello", "letter", "ls", "ps", "sh", "wc"]
    wait_prompt(m)
    type_text(m, "ls /bin\n")
    # 输出比窗口高，命令那一行和头两项会滚出去，只能看最后两行：最后一个文件名和新的提示符
    m.wait_for(lambda mm: console(mm)[-1] == "$" and console(mm)[-2].endswith("  wc"), timeout=5, what="ls /bin 运行完")
    rows = m.screen(25)
    window = rows[8:19]
    check(window[-1] == "$", f"第 18 行应是新的提示符：{window[-1]!r}")
    got = [r.split()[-1] for r in window[:-1]]
    check(got == names[2:], f"第 8～17 行应是 ls /bin 12 行输出的后 10 行（命令那一行和 . .. 已经滚出窗口）：{window}")
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
        # 任务 B 先结束，任务 A 写完 5 个 A 后结束，它的消息写在后面
        m.wait_for(lambda mm: message_row(mm) == "task 1 exited with code 1", what="任务 A 的结束消息")
        k1 = ticks_value(m)
        time.sleep(1.0)
        check(ticks_value(m) - k1 >= 50, "所有任务结束后定时器应照常工作")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")
        check(task_output(m) == "AbyeAAAA",
              f"任务 A 写一个 A 就去睡，任务 B 趁机写完 bye 结束，A 再写完剩下的 4 个：{task_output(m)!r}")
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
        match = re.fullmatch(r"memory: (\d+)/3840 pages free  page faults: (\d+)  copy-on-write: \d+", row3)
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
    free_before = m.screen(4)[3].split("/")[0]
    check(run_command(m, "nosuch") == ["sh: nosuch: not found"], "找不到的命令应报告 not found")
    check(run_command(m, "check") == ["check ok"], "check 应通过")
    out = run_command(m, "check crash")
    check(out == ["sh: check killed by exception 14"], f"check crash 写代码页，应被缺页异常结束：{out!r}")
    out = run_command(m, "ps")
    check(out[0] == "PID PPID S NAME", f"ps 的表头不对：{out!r}")
    rows = {r.split()[0]: r.split() for r in out[1:]}
    expected = {"0": ["0", "0", "R", "idle"], "1": ["1", "0", "S", "task_a"], "2": ["2", "0", "S", "init"],
                "3": ["3", "2", "S", "letter"], "4": ["4", "2", "W", "sh"]}
    for pid, want in expected.items():
        check(rows.get(pid) == want, f"ps 中任务 {pid} 应为 {want}：{out!r}")
    check(any(r[3] == "ps" and r[2] == "R" and r[1] == "4" for r in rows.values()), f"ps 应列出自己：{out!r}")
    m.wait_for(lambda mm: mm.screen(4)[3].split("/")[0] == free_before, timeout=2,
               what=f"命令都结束后空闲页回到 {free_before}")

    check(run_command(m, "ls /bin | wc") == ["12 48 230"], "ls /bin 的输出经管道交给 wc：12 行、48 个词、230 个字节")
    check(run_command(m, "cat /hello.txt | grep Minix") == ["Hello from the Minix file system on hda!"], "grep 从管道读")
    out = run_command(m, "ps | grep sh")
    # ps 运行时，grep 那个子进程可能还没来得及 exec，名字仍是 sh，所以可能多出一行
    check(" 4    2 W sh" in out and all(l.endswith(" sh") for l in out) and len(out) <= 2, f"ps 的输出经管道交给 grep：{out!r}")
    check(run_command(m, "ls /bin | grep e | wc") == ["5 20 104"], "三段的管道：含 e 的 5 个文件名")
    check(run_command(m, "cat /big | wc") == ["2400 7201 614400"], "600 KB 经过只有一页的管道，写满了就等，一个字节也不丢")
    wait_prompt(m)
    type_text(m, "wc\nhello world\n")
    m.wait_for(lambda mm: last_line(mm) == "hello world", timeout=2, what="wc 从键盘读")
    m.sendkeys(["ctrl-d"])
    m.wait_for(lambda mm: console(mm)[-2:] == ["1 2 12", "$"], timeout=2, what="Ctrl+D 结束输入，wc 打印 1 2 12")
    m.wait_for(lambda mm: mm.screen(4)[3].split("/")[0] == free_before, timeout=2,
               what=f"管道和命令都结束后空闲页回到 {free_before}")

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


# 前几项共用一台正常启动的虚拟机；其余各自启动自检内核。
SHARED_TESTS = [test_boot_screen, test_kernel_in_memory, test_protected_mode, test_paging,
                test_timer_rate, test_keyboard_echo, test_keyboard_shift, test_console_scroll,
                test_idle_in_hlt, test_demand_paging, test_fork_address_spaces, test_syscall_gate, test_user_setup, test_user_task,
                test_tasks_alternate, test_shell]
SEPARATE_TESTS = [test_syscalls, test_page_alloc, test_double_free, test_hd_rw, test_early_key, test_keyboard_edge_cases, test_divide_error, test_bad_gate, test_register_dump, test_unmapped_read, test_null_write,
                  test_clobber_loader, test_other_segments,
                  test_big_kernel, test_user_write_kernel, test_user_privileged,
                  test_user_bad_pointer, test_user_exit, test_all_tasks_exit,
                  test_nmi_in_user_mode, test_user_write_code, test_preemption,
                  test_deep_stack, test_wild_read, test_fork_isolation, test_waitpid, test_file_system, test_exec, test_pipe]


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
