/* date：打印现在的时间（UTC），比如 2026-10-19 05:12:33 UTC。内核开机时从 CMOS 读出时钟，之后靠定时器往后数。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    print_time(time(), 1);
    print(" UTC\n");
    return 0;
}
