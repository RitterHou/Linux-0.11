/* cat：把每个文件的内容原样写到标准输出。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    char buf[512];
    int status = 0;

    for (int i = 1; i < argc; i++) {
        int32_t fd = open(argv[i], 0);
        if (fd < 0) {
            print("cat: ");
            print(argv[i]);
            print(": cannot open\n");
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
