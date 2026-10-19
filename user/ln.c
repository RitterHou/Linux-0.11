/* ln 文件 新名字：给文件再起一个名字（硬链接）。两个名字指向同一个 i 节点，ls 里 i 节点号一样。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        eprint("usage: ln file newname\n");
        return 2;
    }
    if (link(argv[1], argv[2]) < 0) {
        eprint("ln: ");
        eprint(argv[2]);
        eprint(": cannot link\n");
        return 1;
    }
    return 0;
}
