/* rmdir 目录…：删掉空目录。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            eprint("rmdir: ");
            eprint(argv[i]);
            eprint(": cannot remove (not an empty directory?)\n");
            status = 1;
        }
    }
    return status;
}
