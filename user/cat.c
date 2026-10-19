/* cat [文件…]：把每个文件的内容原样写到标准输出。没有给文件就读标准输入，读到 0 为止。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    char buf[512];
    int status = 0;

    if (argc == 1) {
        int32_t n;
        while ((n = read(0, buf, sizeof(buf))) > 0)
            write(1, buf, (uint32_t)n);
    }
    for (int i = 1; i < argc; i++) {
        int32_t fd = open(argv[i], 0);
        if (fd < 0) {
            eprint("cat: ");
            eprint(argv[i]);
            eprint(": cannot open\n");
            status = 1;
            continue;
        }
        int32_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, (uint32_t)n);
        close(fd);
    }
    return status;
}
