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
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


class TestFailure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise TestFailure(message)


class Machine:
    """一台无头 QEMU 虚拟机，用监视器命令观察和操作它。"""

    def __init__(self, image):
        self.sock_path = os.path.join(tempfile.mkdtemp(prefix="kernel-test-"), "monitor.sock")
        self.proc = subprocess.Popen(
            ["qemu-system-i386", "-machine", "pc", "-accel", "tcg", "-m", "16M",
             "-drive", f"file={image},format=raw,if=floppy,index=0", "-boot", "order=a",
             "-display", "none", "-nic", "none", "-serial", "none",
             "-monitor", f"unix:{self.sock_path},server,nowait", "-no-reboot"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.time() + 5
        while not os.path.exists(self.sock_path):
            check(time.time() < deadline, "QEMU 没有创建监视器 socket")
            check(self.proc.poll() is None, "QEMU 启动失败")
            time.sleep(0.05)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.sock_path)
        self.sock.settimeout(0.2)
        self._read_until_prompt()

    def _read_until_prompt(self, timeout=3.0):
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
                break
        return ANSI_ESCAPE.sub("", data)

    def cmd(self, line):
        """执行一条监视器命令，返回它的输出。"""
        check(self.proc.poll() is None, "QEMU 已经退出（可能发生了三重错误）")
        self.sock.sendall((line + "\n").encode())
        return self._read_until_prompt()

    def read_memory(self, addr, length):
        """读取物理内存。"""
        out = bytearray()
        while len(out) < length:
            n = min(512, length - len(out))
            dump = self.cmd(f"xp /{n}xb {addr + len(out):#x}")
            for line in dump.splitlines():
                m = re.match(r"\s*[0-9a-f]+:\s*((?:0x[0-9a-f]{2}\s*)+)$", line)
                if m:
                    out.extend(int(b, 16) for b in m.group(1).split())
        return bytes(out[:length])

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
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate(self):
                return
            time.sleep(0.1)
        raise TestFailure(f"{timeout:.0f} 秒内没有等到{what}")

    def close(self):
        try:
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

def boot_ready(m):
    return m.screen(1)[0].startswith("ABPKCVITU")


def test_boot_screen(m, elf):
    """启动后各行的输出"""
    rows = m.screen(9)
    check(rows[0] == "ABPKCVITU", f"第 0 行应为 'ABPKCVITU'，实际是 {rows[0]!r}")
    check(rows[2] == "Hello from kmain() in C!", f"第 2 行不对：{rows[2]!r}")
    check(re.fullmatch(r"ticks: \d+", rows[4]) is not None, f"第 4 行不对：{rows[4]!r}")
    check(rows[5] == "paging: on, 0xC0000000 -> 0xB8000", f"第 5 行不对：{rows[5]!r}")
    check(rows[6].startswith("tasks: "), f"第 6 行不对：{rows[6]!r}")
    check(rows[8] == "keyboard:", f"第 8 行不对：{rows[8]!r}")


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
    match = re.fullmatch(r"ticks: (\d+)", m.screen(5)[4])
    check(match is not None, "第 4 行没有 ticks 计数")
    return int(match.group(1))


def test_timer_rate(m, elf):
    """定时器约 100 Hz"""
    t1, k1 = time.monotonic(), ticks_value(m)
    time.sleep(2.0)
    t2, k2 = time.monotonic(), ticks_value(m)
    rate = (k2 - k1) / (t2 - t1)
    check(90 <= rate <= 110, f"定时器频率应约为 100 Hz，实测 {rate:.1f} Hz")


def test_keyboard_echo(m, elf):
    """键盘回显、回车和退格"""
    m.sendkeys(["h", "i", "spc", "4", "2", "ret", "o", "k", "x", "backspace",
                "ret", "a", "b", "backspace", "backspace", "backspace", "backspace"])
    time.sleep(0.3)
    rows = m.screen(11)
    check(rows[8] == "keyboard: hi 42", f"第 8 行应为 'keyboard: hi 42'，实际是 {rows[8]!r}")
    check(rows[9] == "ok", f"第 9 行应为 'ok'（x 被退格删掉），实际是 {rows[9]!r}")
    check(rows[10] == "", f"第 10 行的 ab 应被退格删光，且不会删到上一行，实际是 {rows[10]!r}")


def test_keyboard_shift(m, elf):
    """Shift、右 Shift 和 Caps Lock"""
    # 接在上一项之后：光标在第 10 行行首。
    m.sendkeys(["shift-h", "i", "shift-1", "spc", "shift-2", "shift-minus", "shift_r-q", "ret",
                "caps_lock", "a", "b", "shift-c", "1", "caps_lock", "d"])
    time.sleep(0.3)
    rows = m.screen(12)
    check(rows[10] == "Hi! @_Q", f"第 10 行应为 'Hi! @_Q'，实际是 {rows[10]!r}")
    check(rows[11] == "ABc1d", f"第 11 行应为 'ABc1d'（Caps Lock 下 Shift 得到小写，数字不受影响），实际是 {rows[11]!r}")


def test_console_scroll(m, elf):
    """回显窗口（第 8～18 行）写满后向上滚动，窗口外的内容不受影响"""
    before = m.screen(8)
    # 接在上一项之后：光标在第 11 行末尾。先换行，再输入 l1～l12 共 12 行，每行以回车结束。
    keys = ["ret"]
    for i in range(1, 13):
        keys += ["l", str(i % 10), "ret"]
    m.sendkeys(keys)
    time.sleep(0.3)
    rows = m.screen(25)
    expected = [f"l{i % 10}" for i in range(3, 13)] + [""]     # l3 … l2，最后是光标所在的空行
    check(rows[8:19] == expected, f"第 8～18 行应为 {expected}，实际是 {rows[8:19]}")
    for r in (0, 2, 5):
        check(rows[r] == before[r], f"第 {r} 行不应被滚动影响：{before[r]!r} 变成了 {rows[r]!r}")
    check(rows[6].startswith("tasks: ") and rows[4].startswith("ticks: "), "第 4、6 行不应被滚动影响")
    check(all(r == "" for r in rows[19:]), f"第 19～24 行应为空：{rows[19:]}")


def test_idle_in_hlt(m, elf):
    """任务等待时停在 sleep 系统调用的 hlt 上，且中断已打开"""
    table = symbols(elf)
    for _ in range(10):
        regs = m.registers()
        if regs["HLT"] == 1 and in_function(table, "sys_sleep", regs["EIP"]):
            check(regs["EFL"] & 0x200, "空闲时 IF 应为 1")
            return
        time.sleep(0.1)
    raise TestFailure("10 次采样都没有停在 sys_sleep 的 hlt 上")


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
    user_start, user_end = table["__user_start"][0], table["__user_end"][0]
    for addr in range(user_start, user_end, 4096):
        pte = read_u32(m, tables + (addr >> 12) * 4)
        check(pte & 0x4, f"用户程序的页 {addr:#x} 应打开用户位：{pte:#x}")
    for name in ("kmain", "timer_ticks", "idt", "tss"):
        addr = table[name][0]
        pte = read_u32(m, tables + (addr >> 12) * 4)
        check(not pte & 0x4, f"内核的 {name}（{addr:#x}）所在页不应打开用户位：{pte:#x}")
    vga_pte = read_u32(m, tables + (0xB8000 >> 12) * 4)
    check(not vga_pte & 0x4, f"显存所在页不应打开用户位：{vga_pte:#x}")


def test_user_task(m, elf):
    """任务 B 运行在用户态：它的内核栈顶是从特权级 3 进入系统调用时 CPU 压入的现场"""
    table = symbols(elf)
    stacks, size = table["task_stacks"]
    top = stacks + size                 # 任务 B 的内核栈（第二个 4 KB）的顶端
    eip, cs, eflags, esp, ss = (read_u32(m, top - 20 + 4 * i) for i in range(5))
    check(cs == 0x33, f"任务 B 进入内核前的 CS 应为用户代码段 0x33，实际是 {cs:#x}")
    check(ss == 0x3B, f"任务 B 进入内核前的 SS 应为用户数据段 0x3B，实际是 {ss:#x}")
    check(in_function(table, "user_main", eip), f"返回地址 {eip:#x} 应在 user_main 内")
    user_stack, stack_size = table["user_stack"]
    check(user_stack < esp <= user_stack + stack_size, f"用户态 ESP {esp:#x} 应在用户栈内")
    check(eflags & 0x200 and not eflags & 0x3000, f"用户态 EFLAGS 应 IF=1、IOPL=0：{eflags:#x}")
    tss_b = table["tss"][0] + 2 * 104
    esp0, ss0 = read_u32(m, tss_b + 4), read_u32(m, tss_b + 8)
    check(esp0 == top and ss0 == 0x10, f"任务 B 的 TSS 中 ss0:esp0 应为 0x10:{top:#x}，实际是 {ss0:#x}:{esp0:#x}")


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


def test_tasks_alternate(m, elf):
    """两个任务轮流运行：第 6 行 A、B 交替出现，TR 在两个 TSS 之间切换"""
    out1 = task_output(m)
    check(set(out1) <= {"A", "B"}, f"任务输出中只应有 A 和 B：{out1!r}")
    check("A" in out1 and "B" in out1, f"两个任务都应有输出：{out1!r}")
    runs = re.findall(r"A+|B+", out1)
    check(len(runs) >= 4, f"A、B 应多次交替，实际只有 {len(runs)} 段：{out1!r}")
    check(max(len(r) for r in runs) <= 15, f"每段应约 10 个字母（时间片 20 tick、每 2 tick 一个）：{out1!r}")
    time.sleep(0.5)
    check(task_output(m) != out1, "0.5 秒后任务输出应有变化")

    tss_a, tss_b = 0x20, 0x28       # GDT 第 4、5 项
    seen = set()
    for _ in range(20):
        seen.add(m.registers()["TR"])
        if {tss_a, tss_b} <= seen:
            return
        time.sleep(0.05)
    raise TestFailure(f"TR 应在 {tss_a:#x} 和 {tss_b:#x} 之间切换，采样到的是 {sorted(hex(x) for x in seen)}")


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
        m.wait_for(lambda mm: mm.screen(1)[0].startswith("ABPKCVIT") and len(mm.screen(1)[0]) > 9,
                   what="第 0 行第 9 列的标记")
        row0 = m.screen(1)[0]
        check(row0[9] == "Y", f"内核末尾的标记应为 Y，第 0 行是 {row0!r}")
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
    check(m.registers()["TR"] == 0x20, "任务 B 结束后应只剩任务 A（TR=0x20）在运行")


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
        m.wait_for(lambda mm: message_row(mm) == "task 2 exited with code 2", what="任务 B 的结束消息")
        k1 = ticks_value(m)
        time.sleep(1.0)
        check(ticks_value(m) - k1 >= 50, "所有任务结束后定时器应照常工作")
        check(not exception_screen(m)[0], f"不应出现异常面板：{exception_screen(m)[0]!r}")
        check(task_output(m).startswith("AAAAAbye"), f"任务 A 写 5 个 A 后结束，任务 B 写 bye 后结束：{task_output(m)!r}")
        for _ in range(10):
            regs = m.registers()
            if regs["HLT"] == 1:
                break
            time.sleep(0.05)
        check(regs["TR"] == 0x18, f"应切回任务 0（TR=0x18），实际 TR={regs['TR']:#x}")
        check(in_function(table, "kmain", regs["EIP"]), f"应停在 kmain 的 hlt 上，EIP={regs['EIP']:#x}")
        check(regs["CR3"] == table["page_directory"][0], f"CR3 应仍是页目录，实际是 {regs['CR3']:#x}")


def test_early_key(_m, _elf):
    """初始化 8259 之前就有按键：键盘之后仍能正常回显"""
    d = build_selftest("EARLY_KEY")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: mm.screen(1)[0][9:10] == "W", what="等待按键的标记 W")
        m.sendkeys(["x"])
        m.wait_for(boot_ready, what="启动完成")
        m.sendkeys(["h", "i"])
        time.sleep(0.3)
        row = m.screen(9)[8]
        check(row == "keyboard: hi", f"开中断前的按键被丢弃后，键盘应照常回显：第 8 行是 {row!r}")


# 前几项共用一台正常启动的虚拟机；其余各自启动自检内核。
SHARED_TESTS = [test_boot_screen, test_kernel_in_memory, test_protected_mode, test_paging,
                test_timer_rate, test_keyboard_echo, test_keyboard_shift, test_console_scroll,
                test_idle_in_hlt, test_syscall_gate, test_user_setup, test_user_task,
                test_tasks_alternate]
SEPARATE_TESTS = [test_syscalls, test_early_key, test_divide_error, test_bad_gate, test_register_dump, test_unmapped_read, test_null_write,
                  test_clobber_loader, test_other_segments,
                  test_big_kernel, test_user_write_kernel, test_user_privileged,
                  test_user_bad_pointer, test_user_exit, test_all_tasks_exit]


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
        print(f"{status}  {test.__doc__}（{time.monotonic() - start:.1f}s）{detail}", flush=True)

    with Machine(image) as m:
        try:
            m.wait_for(boot_ready, what="启动完成（第 0 行出现 ABPKCVITU）")
        except TestFailure as e:
            print(f"FAIL  启动：{e}")
            sys.exit(1)
        for test in SHARED_TESTS:
            run(test, m)
    for test in SEPARATE_TESTS:
        run(test, None)

    total = len(SHARED_TESTS) + len(SEPARATE_TESTS)
    print(f"\n{total - failures}/{total} 项通过")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
