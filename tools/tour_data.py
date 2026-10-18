"""把最新构建出的数字回填进介绍网页 docs/tour.html：先 make，再运行

    python3 tools/tour_data.py docs/tour.html

页面脚本里有三个对象由这里生成，其余代码只引用它们：
  FACTS  内核的大小、各段和符号的地址、代码行数、提交数、测试数（取自 build/kernel.elf 和 git）
  DISK   硬盘镜像里的 i 节点号、块号、df 的数字、ls /bin 的输出，sh、check、sort 里的几个地址
         （取自 build/hd.img 和 build/user/ 下的 ELF 文件）
  FS0    头图里模拟的 shell 用的文件系统快照：每个 i 节点的类型和大小、每个目录的目录项、小文本文件的内容
/bin 里每多一个程序，后面文件的 i 节点号和块号都会变，所以这些数字都不要写死在页面里。
"""
import json
import re
import struct
import subprocess
import sys

page = sys.argv[1]


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


# ---------------------------------------------------------------- FACTS
sections, syms = {}, {}
for line in run("objdump", "-h", "build/kernel.elf").splitlines():
    m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})", line)
    if m:
        sections[m.group(1)] = int(m.group(3), 16)
for line in run("objdump", "-t", "build/kernel.elf").splitlines():
    m = re.match(r"([0-9a-f]{8})\s.*\s([0-9a-f]{8})\s+(\S+)$", line)
    if m:
        syms[m.group(3)] = int(m.group(1), 16)
src = ["boot.asm", "loader.asm", "entry.asm", "isr.asm", "linker.ld"] + run("git", "ls-files", "*.c", "*.h").split()
loc = sum(len(open(f).read().splitlines()) for f in src)
kernel_size = len(open("build/kernel.bin", "rb").read())
facts = {
    "head": run("git", "rev-parse", "--short", "HEAD").strip(), "loc": f"{loc:,}", "kernelKB": f"{kernel_size // 1024} KB",
    "kernelSectors": (kernel_size + 511) // 512,
    "commits": len(run("git", "log", "--format=%h", "main..HEAD").split()) - 1,     # 去掉最初的空提交
    "tests": len(re.findall(r"^def test_", open("tests/test_kernel.py").read(), re.M)),
    "selftests": len(re.findall(r"^#define SELFTEST_[A-Z_]+\s+\d+", open("selftest.h").read(), re.M)),
}
for key, value in [("text", sections[".text"]), ("rodata", sections[".rodata"]), ("data", sections[".data"]),
                   ("user", syms["__user_start"]), ("userData", syms["__user_data"]), ("userEnd", syms["__user_end"]),
                   ("bss", syms["__bss_start"]), ("bssEnd", syms["__bss_end"]), ("stackTop", syms["stack_top"]),
                   ("gdt", syms["gdt"]), ("idt", syms["idt"]), ("identity", syms["identity_tables"]),
                   ("pageDir", syms["page_directory"]), ("alias", syms["alias_table"]), ("ticks", syms["timer_ticks"]),
                   ("tss", syms["tss"]), ("userMain", syms["user_main"]), ("kmain", syms["kmain"])]:
    facts[key] = f"0x{value:x}"          # 写成十六进制字面量，页面里当数用

# ---------------------------------------------------------------- 硬盘镜像
img = open("build/hd.img", "rb").read()
blk = lambda n: img[n * 1024:(n + 1) * 1024]
ninodes, nzones, imap, zmap, firstdata, _, _, magic = struct.unpack_from("<HHHHHHIH", blk(1))
assert magic == 0x137F, "build/hd.img 不是 Minix v1 文件系统"
itable = 2 + imap + zmap


def free_bits(first, count, limit):
    data = b"".join(blk(first + i) for i in range(count))
    return [b for b in range(1, limit + 1) if not data[b // 8] & (1 << (b % 8))]


free_inodes = free_bits(2, imap, ninodes)
free_zones = free_bits(2 + imap, zmap, nzones - firstdata)


def inode(n):
    off = (n - 1) * 32
    b = blk(itable + off // 1024)
    mode, _, size = struct.unpack_from("<HHI", b, off % 1024)
    return mode, size, struct.unpack_from("<9H", b, off % 1024 + 14)


def blocks_of(zones):
    """i 节点占的所有块：数据块，以及间接块、二级间接块本身。"""
    out = [z for z in zones[:7] if z]
    if zones[7]:
        out += [zones[7]] + [z for z in struct.unpack_from("<512H", blk(zones[7])) if z]
    if zones[8]:
        out.append(zones[8])
        for ind in struct.unpack_from("<512H", blk(zones[8])):
            if ind:
                out += [ind] + [z for z in struct.unpack_from("<512H", blk(ind)) if z]
    return out


def content(zones, size):
    return b"".join(blk(z) for z in zones[:7] if z)[:size]


kind = lambda mode: {0o040000: "d", 0o020000: "c"}.get(mode & 0o170000, "-")
inodes, dirs, texts, paths, owner = {}, {}, {}, {}, {}


def walk(n, path):
    mode, size, zones = inode(n)
    inodes[n], paths[n] = [kind(mode), size], path
    if kind(mode) != "c":
        for b in blocks_of(zones):
            owner[b] = path
    if kind(mode) == "-" and size < 200:
        texts[n] = content(zones, size).decode()
    if kind(mode) != "d":
        return
    data = content(zones, size)
    dirs[n] = []
    for i in range(0, len(data), 16):
        ino, = struct.unpack_from("<H", data, i)
        name = data[i + 2:i + 16].rstrip(b"\0").decode()
        if ino:
            dirs[n].append([name, ino])
            if name not in (".", "..") and ino not in inodes:
                walk(ino, (path.rstrip("/") + "/" + name))


walk(1, "/")
ino = {p: n for n, p in paths.items()}
first_block = lambda path: blocks_of(inode(ino[path])[2])[0]
new_ino, new_bit = free_inodes[0], free_zones[0]
new_block = firstdata + new_bit - 1
window = list(range(new_block - 5, new_block))
disk = {
    "firstData": firstdata, "itable": [itable, itable + (ninodes * 32 + 1023) // 1024 - 1], "imap": 2, "zmap": 2 + imap,
    "ino": {k: ino[p] for k, p in [("bin", "/bin"), ("dev", "/dev"), ("etc", "/etc"), ("issue", "/etc/issue"),
                                   ("hello", "/hello.txt"), ("tmp", "/tmp"), ("big", "/big")]},
    "blk": {"root": first_block("/"), "bin": first_block("/bin"), "etc": first_block("/etc"),
            "issue": first_block("/etc/issue"), "tmp": first_block("/tmp"),
            "big": [min(blocks_of(inode(ino["/big"])[2])), max(blocks_of(inode(ino["/big"])[2]))]},
    "root": dirs[1], "etc": dirs[ino["/etc"]], "issueSize": inodes[ino["/etc/issue"]][1], "tmpSize": inodes[ino["/tmp"]][1],
    "newIno": new_ino, "newInoBlock": itable + (new_ino - 1) // 32, "newBit": new_bit, "newBlock": new_block,
    "owners": [[b, owner.get(b, "")] for b in window],
    "df": {"blocks": nzones - firstdata, "used": nzones - firstdata - len(free_zones),
           "inodes": ninodes, "inodesUsed": ninodes - len(free_inodes)},
    "lsBin": [f"{inodes[n][0]}{inodes[n][1]:>8}{n:>4}  {name}" for name, n in dirs[ino["/bin"]]],
}
assert owner.get(new_block - 1) and not owner.get(new_block), "区块位图和文件占的块对不上"

# ---------------------------------------------------------------- 程序里的地址
def symbol(elf, name):
    for line in run("objdump", "-t", elf).splitlines():
        if line.split()[-1:] == [name]:
            return int(line.split()[0], 16)
    sys.exit(f"{elf} 里没有 {name}")


asm = run("objdump", "-d", "--no-show-raw-insn", "build/user/sh.elf").splitlines()
wait_ret = next(int(asm[i + 1].split(":")[0], 16) for i, line in enumerate(asm[:-1])
                if "int\t$0x80" in line and "cmpl\t$-0x4, %eax" in asm[i + 1])     # shell 调用 waitpid 后检查 -EINTR
check_main = symbol("build/user/check.elf", "main")
crash = next(int(line.split(":")[0], 16) for line in run("objdump", "-d", "--no-show-raw-insn", "build/user/check.elf").splitlines()
             if re.search(rf"movb\s+\$0x0, 0x{check_main:x}$", line))             # check crash 写自己的代码页
disk.update({"sortEnd": f"0x{symbol('build/user/sort.elf', '__end'):08X}",           # sort 的堆从这里开始
             "onIntr": f"0x{symbol('build/user/sh.elf', 'on_intr'):08X}", "sigRestore": f"0x{symbol('build/user/sh.elf', 'sig_restore'):08X}",
             "waitRet": f"0x{wait_ret:08X}", "crashEip": f"0x{crash:08X}"})

fs0 = {"ninodes": ninodes, "blocks": nzones - firstdata, "nFreeBlocks": len(free_zones), "inodes": inodes, "dirs": dirs, "texts": texts}

# ---------------------------------------------------------------- 写回页面
html = open(page).read()
m = re.search(r"const FACTS = \{.*?\n\};", html, re.S)
if not m:
    sys.exit("页面里没有 FACTS 对象")
block = m.group(0)
for key, value in facts.items():
    literal = json.dumps(value) if key in ("head", "loc", "kernelKB") else str(value)   # 地址是十六进制数，不加引号
    block, n = re.subn(rf'(\b{key}: )("[^"]*"|[^,\n]+)', lambda mm: mm.group(1) + literal, block, count=1)
    if n != 1:
        sys.exit(f"FACTS 中没有 {key}")
html = html[:m.start()] + block + html[m.end():]
for name, value in (("DISK", disk), ("FS0", fs0)):
    html, n = re.subn(rf"const {name} = \{{.*?\}};\n", lambda _: f"const {name} = " + json.dumps(value, ensure_ascii=False, separators=(",", ":")) + ";\n",
                      html, count=1, flags=re.S)
    if n != 1:
        sys.exit(f"页面里没有 {name} 对象")
# 静态写在 HTML 里的初始值也一起更新：JS 加载前就能看到正确的数字
for key in ("loc", "kernelKB", "commits", "tests", "kernelSectors", "head", "selftests"):
    html = re.sub(rf'(data-fact="{key}"[^>]*>)[^<]*(<)', rf"\g<1>{facts[key]}\g<2>", html)
open(page, "w").write(html)
print(json.dumps({**facts, **{k: v for k, v in disk.items() if k not in ("root", "etc", "lsBin")}}, ensure_ascii=False))
