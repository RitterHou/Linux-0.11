/* mkdir 目录…：新建目录。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i]) < 0) {
            eprint("mkdir: ");
            eprint(argv[i]);
            eprint(": cannot create\n");
            status = 1;
        }
    }
    return status;
}
