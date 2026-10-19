/* touch 文件…：文件不存在就新建一个空的，存在就把修改时间改成现在。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        int32_t fd;
        if (stat(argv[i], &st) == 0 ? utime(argv[i], 0) < 0 : (fd = open(argv[i], O_WRONLY | O_CREAT)) < 0 || close(fd) < 0) {
            print("touch: ");
            print(argv[i]);
            print(": cannot touch\n");
            status = 1;
        }
    }
    return status;
}
