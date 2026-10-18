/* ps：列出所有任务：任务号、父进程、状态、名字。状态：R 能运行，S 在 sleep 里，Z 僵尸，W 在 waitpid 里，B 在等待队列上（等键盘、等硬盘）。 */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    static const char states[] = "?RSZWB";
    struct taskinfo info;

    print("PID PPID S NAME\n");
    for (int pid = 0; pid < 16; pid++) {
        if (taskinfo(pid, &info) < 0)
            continue;
        if (pid < 10)
            print(" ");
        print_num((uint32_t)pid);
        print("   ");
        if (info.parent < 10)
            print(" ");
        print_num((uint32_t)info.parent);
        char s[4] = { ' ', info.state >= 1 && info.state <= 5 ? states[info.state] : '?', ' ', '\0' };
        print(s);
        print(info.name);
        print("\n");
    }
    return 0;
}
