/* rmdir 目录…：删掉空目录。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            print("rmdir: ");
            print(argv[i]);
            print(": cannot remove (not an empty directory?)\n");
            status = 1;
        }
    }
    return status;
}
