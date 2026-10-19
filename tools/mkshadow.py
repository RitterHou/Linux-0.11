"""生成 /etc/shadow：每行是 名字:密码的散列。散列的算法和 user/ulib.c 的 hash_password() 一样。

Usage: python3 tools/mkshadow.py <输出文件> 名字:密码 ...

散列是 64 位的 FNV-1a，以用户名作盐（同样的密码，不同的用户散列不同），对“名字:密码”重复算 1000 遍，
写成 16 个十六进制数字。这只是为了演示“只存散列、不存密码”：真正的系统要用专门设计得很慢的算法（bcrypt、yescrypt），
还要随机的盐，FNV 这样的快速散列很容易被暴力试出来。
"""
import sys

MASK = (1 << 64) - 1


def hash_password(name, password):
    data = (name + ":" + password).encode()
    h = 0xCBF29CE484222325
    for _ in range(1000):
        for b in data:
            h ^= b
            h = (h * 0x100000001B3) & MASK
    return f"{h:016x}"


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "w") as f:
        for item in sys.argv[2:]:
            name, password = item.split(":", 1)
            f.write(f"{name}:{hash_password(name, password)}\n")


if __name__ == "__main__":
    main()
