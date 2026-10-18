/* hello：打印 hello，再把参数一个个打印出来，用空格隔开。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    print("hello");
    for (int i = 1; i < argc; i++) {
        print(" ");
        print(argv[i]);
    }
    return 0;
}
