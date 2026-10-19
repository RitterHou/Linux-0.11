/* chmod 八进制的权限 文件…：改文件的权限，比如 chmod 755 脚本，让 #! 开头的脚本能直接运行。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    uint32_t mode = 0;
    const char *p = argc >= 3 ? argv[1] : "";

    if (*p == '\0') {
        print("usage: chmod mode file...\n");
        return 2;
    }
    for (; *p; p++) {
        if (*p < '0' || *p > '7') {
            print("usage: chmod mode file...\n");
            return 2;
        }
        mode = mode * 8 + (uint32_t)(*p - '0');
    }
    int status = 0;
    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], mode) < 0) {
            print("chmod: ");
            print(argv[i]);
            print(": cannot change mode\n");
            status = 1;
        }
    }
    return status;
}
