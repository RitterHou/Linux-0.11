/* rm 文件…：删掉文件。目录要用 rmdir。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) < 0) {
            eprint("rm: ");
            eprint(argv[i]);
            eprint(": cannot remove\n");
            status = 1;
        }
    }
    return status;
}
