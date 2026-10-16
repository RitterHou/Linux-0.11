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
    return m.screen(1)[0].startswith("ABPKCVIT")


def test_boot_screen(m, elf):
    """启动后各行的输出"""
    rows = m.screen(9)
    check(rows[0] == "ABPKCVIT", f"第 0 行应为 'ABPKCVIT'，实际是 {rows[0]!r}")
    check(rows[2] == "Hello from kmain() in C!", f"第 2 行不对：{rows[2]!r}")
    check(re.fullmatch(r"ticks: \d+", rows[4]) is not None, f"第 4 行不对：{rows[4]!r}")
    check(rows[5] == "paging: on, 0xC0000000 -> 0xB8000", f"第 5 行不对：{rows[5]!r}")
    check(rows[6].startswith("tasks: "), f"第 6 行不对：{rows[6]!r}")
    check(rows[8] == "keyboard:", f"第 8 行不对：{rows[8]!r}")


def test_kernel_in_memory(m, elf):
    """0x10000 处的内存与 kernel.bin 一致"""
    with open(os.path.join(BUILD, "kernel.bin"), "rb") as f:
        expected = f.read()
    actual = m.read_memory(0x10000, len(expected))
    check(actual == expected, f"0x10000 处的 {len(expected)} 字节与 kernel.bin 不一致")


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


def test_idle_in_hlt(m, elf):
    """任务等待时停在 hlt 上，且中断已打开"""
    table = symbols(elf)
    for _ in range(10):
        regs = m.registers()
        in_task = in_function(table, "task_a", regs["EIP"]) or in_function(table, "task_b", regs["EIP"])
        if regs["HLT"] == 1 and in_task:
            check(regs["EFL"] & 0x200, "空闲时 IF 应为 1")
            return
        time.sleep(0.1)
    raise TestFailure("10 次采样都没有停在任务 A 或 B 的 hlt 上")


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


def exception_screen(m):
    return m.screen(12)[10:12]


def test_divide_error(_m, _elf):
    """除以零：显示 #DE，EIP 指向 div 指令"""
    d = build_selftest("DIVIDE_ERROR")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: exception_screen(mm)[0].startswith("Exception"), what="异常信息")
        line10, line11 = exception_screen(m)
        check(line10.startswith("Exception 0") and "#DE" in line10, f"第 10 行不对：{line10!r}")
        check(m.attribute(10, 0) == 0x4F, "异常信息应为红底白字")
        eip = re.search(r"EIP=0x([0-9A-F]{8})", line11)
        check(eip is not None, f"第 11 行没有 EIP：{line11!r}")
        insn = instruction_at(os.path.join(d, "kernel.elf"), int(eip.group(1), 16))
        check(insn is not None and insn.startswith("div"), f"EIP 处应是 div 指令，实际是 {insn}")


def test_bad_gate(_m, _elf):
    """触发未填写的 IDT 表项：显示 #GP，错误码 0x30*8+2"""
    d = build_selftest("BAD_GATE")
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: exception_screen(mm)[0].startswith("Exception"), what="异常信息")
        line10, line11 = exception_screen(m)
        check(line10.startswith("Exception 13") and "#GP" in line10, f"第 10 行不对：{line10!r}")
        check("error=0x00000182" in line11, f"错误码应为 0x182：{line11!r}")


def page_fault_test(selftest, error_code, cr2):
    d = build_selftest(selftest)
    with Machine(os.path.join(d, "floppy.img")) as m:
        m.wait_for(lambda mm: exception_screen(mm)[0].startswith("Exception"), what="异常信息")
        line10, line11 = exception_screen(m)
        check(line10.startswith("Exception 14") and "#PF" in line10, f"第 10 行不对：{line10!r}")
        check(f"error=0x{error_code:08X}" in line11, f"错误码应为 {error_code:#x}：{line11!r}")
        check(f"CR2=0x{cr2:08X}" in line11, f"CR2 应为 {cr2:#x}：{line11!r}")


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
                    check(regs[seg] == 0x38, f"中断返回后 {seg} 应恢复为 0x38，实际是 {regs[seg]:#x}")
                return
            time.sleep(0.1)
        raise TestFailure("10 次采样都没有停在 selftest_run 的 hlt 上")


# 前几项共用一台正常启动的虚拟机；其余各自启动自检内核。
SHARED_TESTS = [test_boot_screen, test_kernel_in_memory, test_protected_mode, test_paging,
                test_timer_rate, test_keyboard_echo, test_idle_in_hlt, test_tasks_alternate]
SEPARATE_TESTS = [test_divide_error, test_bad_gate, test_unmapped_read, test_null_write,
                  test_clobber_loader, test_other_segments]


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
            m.wait_for(boot_ready, what="启动完成（第 0 行出现 ABPKCVIT）")
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
