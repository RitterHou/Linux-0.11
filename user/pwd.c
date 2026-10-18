/* pwd：打印当前目录。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    char buf[128];

    if (getcwd(buf, sizeof(buf)) < 0)
        return 1;
    print(buf);
    print("\n");
    return 0;
}
