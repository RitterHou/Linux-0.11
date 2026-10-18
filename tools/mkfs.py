"""把一个目录打包成 Minix v1 文件系统镜像，格式与 Linux 0.11 使用的相同（include/linux/fs.h）。

Usage: python3 tools/mkfs.py <image> <dir> [size_kb]

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
import sys

BLOCK = 1024
NAME_LEN = 14
MAGIC = 0x137F
NINODES = 128
S_IFDIR, S_IFREG = 0o040000, 0o100000
MAX_SIZE = (7 + 512 + 512 * 512) * BLOCK


def ceil_div(a, b):
    return (a + b - 1) // b


def main():
    image, src = sys.argv[1], sys.argv[2]
    nzones = int(sys.argv[3]) if len(sys.argv) > 3 else 4096     # 总块数，含元数据
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

    # 先按目录树给每个文件和目录编好 i 节点号，目录内容要用到子项的编号
    nodes = []                          # (路径, 是否目录)
    def walk(path):
        nodes.append((path, True))
        for name in sorted(os.listdir(path)):
            if len(name.encode()) > NAME_LEN:
                sys.exit(f"文件名超过 14 个字节：{name}")
            full = os.path.join(path, name)
            if os.path.isdir(full):
                walk(full)
            else:
                nodes.append((full, False))
    walk(src)
    if len(nodes) > NINODES:
        sys.exit("文件太多，i 节点不够")
    ino_of = {path: i + 1 for i, (path, _) in enumerate(nodes)}
    parent_of = {src: src}
    for path, _ in nodes:
        if path != src:
            parent_of[path] = os.path.dirname(path)

    inodes = {}
    for path, is_dir in nodes:
        if is_dir:
            children = sorted(os.listdir(path))
            entries = [(".", ino_of[path]), ("..", ino_of[parent_of[path]])]
            entries += [(name, ino_of[os.path.join(path, name)]) for name in children]
            content = b"".join(struct.pack("<H14s", ino, name.encode()) for name, ino in entries)
            subdirs = sum(os.path.isdir(os.path.join(path, n)) for n in children)
            mode, nlinks = S_IFDIR | 0o755, 2 + subdirs
        else:
            with open(path, "rb") as f:
                content = f.read()
            if len(content) > MAX_SIZE:
                sys.exit(f"文件太大：{path}")
            mode = S_IFREG | (0o755 if os.access(path, os.X_OK) else 0o644)
            nlinks = 1
        inodes[ino_of[path]] = struct.pack("<HHIIBB9H", mode, 0, len(content), 0, 0, nlinks, *store(content))

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
