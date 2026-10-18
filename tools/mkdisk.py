"""生成 1 MB 的测试硬盘镜像：第 k 个扇区以 "SECTOR %08d" 开头，其余字节是 k 的低 8 位。

Usage: python3 tools/mkdisk.py <hd.img>
"""
import sys

SECTORS = 2048
with open(sys.argv[1], "wb") as f:
    for k in range(SECTORS):
        head = b"SECTOR %08d" % k
        f.write(head + bytes([k & 0xFF]) * (512 - len(head)))
