"""生成测试用的大文件：600 KB，第 i 个字节是 (i * 7 + 3) & 0xFF。
大于 7 + 512 个块，读它要用到 i 节点的二级间接块。

Usage: python3 tools/mkbig.py <file>
"""
import sys

SIZE = 600 * 1024
with open(sys.argv[1], "wb") as f:
    f.write(bytes((i * 7 + 3) & 0xFF for i in range(SIZE)))
