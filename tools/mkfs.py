"""把一个目录打包成 Minix v1 文件系统镜像，格式与 Linux 0.11 使用的相同（include/linux/fs.h）。

Usage: python3 tools/mkfs.py <image> <dir> [--chr 路径=主设备号,次设备号 ...] [--own 路径=主人,组,权限 ...]

--chr 在镜像里加一个字符设备文件，例如 --chr /dev/tty0=4,0：普通用户在 macOS 上建不了设备文件，
所以不放在目录里，由这里直接写进镜像。设备号存在 i 节点的第一个区块号里，与 Linux 0.11 相同。
--own 设一个文件的主人、组和权限（八进制，可以带 setuid、setgid、粘滞位），例如 --own /tmp=0,0,1777：
目录里的文件在宿主机上都是自己的，权限也不一定是想要的。没写到的文件主人和组都是 0（root），
目录 0755，有执行权限的文件 0755，别的 0644。

布局（块大小 1024 字节）：
  块 0          引导块，这里写一行说明文字
  块 1          超级块
  块 2 起       i 节点位图、区块位图、i 节点表
  s_firstdatazone 起是数据区块：文件内容、目录项、间接块
i 节点从 1 开始编号，1 号是根目录。每个 i 节点有 9 个区块号：前 7 个直接指向数据，
第 8 个指向一个间接块（512 个区块号），第 9 个指向二级间接块。
"""
import os
import struct
import time
import sys

BLOCK = 1024
NAME_LEN = 14
MAGIC = 0x137F
NINODES = 128
S_IFDIR, S_IFREG, S_IFCHR = 0o040000, 0o100000, 0o020000
NZONES = 4096                           # 镜像的总块数（含元数据），4 MB
MAX_SIZE = (7 + 512 + 512 * 512) * BLOCK


def ceil_div(a, b):
    return (a + b - 1) // b


def main():
    image, src = sys.argv[1], sys.argv[2]
    devices = {}                        # 镜像里的路径（不以 / 开头）-> 设备号
    owners = {}                         # 镜像里的路径 -> (主人, 组, 权限)
    args = sys.argv[3:]
    while args:
        if args[0] not in ("--chr", "--own") or len(args) < 2:
            sys.exit(__doc__)
        path, numbers = args[1].split("=")
        if args[0] == "--chr":
            major, minor = (int(x) for x in numbers.split(","))
            devices[path.strip("/")] = major << 8 | minor
        else:
            uid, gid, perm = numbers.split(",")
            owners[path.strip("/")] = (int(uid), int(gid), int(perm, 8))
        args = args[2:]
    nzones = NZONES
    imap_blocks = ceil_div(NINODES + 1, BLOCK * 8)
    zmap_blocks = ceil_div(nzones + 1, BLOCK * 8)
    inode_blocks = ceil_div(NINODES * 32, BLOCK)
    first_data = 2 + imap_blocks + zmap_blocks + inode_blocks

    blocks = {}                         # 区块号 -> 1024 字节
    next_zone = [first_data]

    def alloc(data):
        zone = next_zone[0]
        if zone >= nzones:
            sys.exit("镜像太小，放不下这些文件")
        next_zone[0] += 1
        blocks[zone] = data.ljust(BLOCK, b"\0")
        return zone

    def store(content):
        """写入文件内容，返回 9 个区块号。"""
        zones = [alloc(content[i:i + BLOCK]) for i in range(0, len(content), BLOCK)]
        izone = zones[:7] + [0] * (7 - len(zones[:7]))
        rest = zones[7:]
        indirect = double = 0
        if rest:
            indirect = alloc(struct.pack("<512H", *(rest[:512] + [0] * (512 - len(rest[:512])))))
            rest = rest[512:]
        if rest:
            tables = []
            for i in range(0, len(rest), 512):
                chunk = rest[i:i + 512]
                tables.append(alloc(struct.pack("<512H", *(chunk + [0] * (512 - len(chunk))))))
            double = alloc(struct.pack("<512H", *(tables + [0] * (512 - len(tables)))))
        return izone + [indirect, double]

    # 先把目录树读成 路径 -> 内容 的表（路径相对于镜像的根，根目录是 ""），再加上设备文件
    tree = {"": "dir"}
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        rel = "" if rel == "." else rel
        for name in dirs + files:
            if len(name.encode()) > NAME_LEN:
                sys.exit(f"文件名超过 14 个字节：{name}")
        for name in dirs:
            tree[os.path.join(rel, name)] = "dir"
        for name in files:
            tree[os.path.join(rel, name)] = os.path.join(root, name)
    for path, dev in devices.items():
        parent = os.path.dirname(path)
        while parent and parent not in tree:   # 设备文件所在的目录不存在就建上
            tree[parent] = "dir"
            parent = os.path.dirname(parent)
        tree[path] = dev

    def children(path):
        return sorted(p for p in tree if p and os.path.dirname(p) == path)

    # 按目录树的顺序给每一项编好 i 节点号，目录内容要用到子项的编号
    order = []
    def walk(path):
        order.append(path)
        for child in children(path):
            if tree[child] == "dir":
                walk(child)
            else:
                order.append(child)
    walk("")
    if len(order) > NINODES:
        sys.exit("文件太多，i 节点不够")
    ino_of = {path: i + 1 for i, path in enumerate(order)}

    for path in owners:
        if path not in tree:
            sys.exit(f"--own 的文件不存在：/{path}")
    inodes = {}
    now = int(time.time())
    for path in order:
        kind = tree[path]
        zones = [0] * 9
        if kind == "dir":
            entries = [(".", ino_of[path]), ("..", ino_of[os.path.dirname(path) if path else ""])]
            entries += [(os.path.basename(c), ino_of[c]) for c in children(path)]
            content = b"".join(struct.pack("<H14s", ino, name.encode()) for name, ino in entries)
            subdirs = sum(tree[c] == "dir" for c in children(path))
            mode, nlinks, zones = S_IFDIR | 0o755, 2 + subdirs, store(content)
        elif isinstance(kind, int):    # 字符设备：没有内容，第一个区块号是设备号
            content, mode, nlinks = b"", S_IFCHR | 0o666, 1
            zones[0] = kind
        else:
            with open(kind, "rb") as f:
                content = f.read()
            if len(content) > MAX_SIZE:
                sys.exit(f"文件太大：{kind}")
            mode = S_IFREG | (0o755 if os.access(kind, os.X_OK) else 0o644)
            nlinks, zones = 1, store(content)
        # 修改时间：普通文件取源文件的，目录和设备文件取打包的时刻
        mtime = int(os.stat(kind).st_mtime) if isinstance(kind, str) and kind != "dir" else now
        uid, gid = 0, 0
        if path in owners:
            uid, gid, perm = owners[path]
            mode = (mode & 0o170000) | perm
        inodes[ino_of[path]] = struct.pack("<HHIIBB9H", mode, uid, len(content), mtime, gid, nlinks, *zones)
    nodes = order

    # 位图：第 0 位保留不用，置 1；超出范围的位也置 1，免得被当成空闲
    def bitmap(nblocks, used_bits, total_bits):
        bits = bytearray(nblocks * BLOCK)
        for i in range(nblocks * BLOCK * 8):
            if i == 0 or i <= used_bits or i > total_bits:
                bits[i // 8] |= 1 << (i % 8)
        return bytes(bits)

    imap = bitmap(imap_blocks, len(nodes), NINODES)
    zmap = bitmap(zmap_blocks, next_zone[0] - first_data, nzones - first_data)
    table = b"".join(inodes.get(i, b"\0" * 32) for i in range(1, NINODES + 1))

    out = bytearray(nzones * BLOCK)
    out[0:BLOCK] = b"minix v1 file system, made by tools/mkfs.py\n".ljust(BLOCK, b"\0")
    out[BLOCK:2 * BLOCK] = struct.pack("<HHHHHHIH", NINODES, nzones, imap_blocks, zmap_blocks,
                                       first_data, 0, MAX_SIZE, MAGIC).ljust(BLOCK, b"\0")
    pos = 2 * BLOCK
    for part in (imap, zmap, table.ljust(inode_blocks * BLOCK, b"\0")):
        out[pos:pos + len(part)] = part
        pos += len(part)
    for zone, data in blocks.items():
        out[zone * BLOCK:(zone + 1) * BLOCK] = data
    with open(image, "wb") as f:
        f.write(out)
    print(f"{image}: {len(nodes)} 个文件和目录，用了 {next_zone[0] - first_data} 个数据块")


if __name__ == "__main__":
    main()
