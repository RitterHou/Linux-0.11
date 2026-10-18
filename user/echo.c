/* echo：把参数用空格连起来打印，最后换行。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            print(" ");
        print(argv[i]);
    }
    print("\n");
    return 0;
}
