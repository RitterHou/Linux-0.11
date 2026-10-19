/* chmod 八进制的权限 文件…：改文件的权限，比如 chmod 755 脚本，让 #! 开头的脚本能直接运行。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    uint32_t mode;

    if (argc < 3 || !parse_uint(argv[1], 8, 07777, &mode)) {
        eprint("usage: chmod mode file...\n");
        return 2;
    }
    int status = 0;
    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], mode) < 0) {
            eprint("chmod: ");
            eprint(argv[i]);
            eprint(": cannot change mode\n");
            status = 1;
        }
    }
    return status;
}
