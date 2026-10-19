/*
 * echo [-n] [-e] 参数…：把参数用空格连起来打印，最后换行。-n 不换行；-e 把 \n、\t、\r、\\ 换成换行、Tab、回车、反斜杠，
 * 键盘上敲不出 Tab 和回车，要写这样的文件就靠它。
 */
#include "ulib.h"

static void print_escaped(const char *s)
{
    for (; *s; s++) {
        char c = *s;
        if (c == '\\' && s[1]) {
            char e = *++s;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
            if (e != 'n' && e != 't' && e != 'r' && e != '\\')
                write(1, "\\", 1);   /* 不认识的 \x 原样打印 */
        }
        write(1, &c, 1);
    }
}

int main(int argc, char **argv)
{
    int newline = 1, escapes = 0, i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0)
            newline = 0;
        else if (strcmp(argv[i], "-e") == 0)
            escapes = 1;
        else
            break;
    }
    for (int first = i; i < argc; i++) {
        if (i > first)
            print(" ");
        if (escapes)
            print_escaped(argv[i]);
        else
            print(argv[i]);
    }
    if (newline)
        print("\n");
    return 0;
}
