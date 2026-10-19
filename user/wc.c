/* wc [文件…]：数行数、词数和字节数。没有给文件就读标准输入，读到 0（管道写的一头关了，或者键盘上按了 Ctrl+D）为止。 */
#include "ulib.h"

static uint32_t lines, words, bytes;

static void count(int32_t fd)
{
    char buf[512];
    int32_t n;
    int in_word = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int32_t i = 0; i < n; i++) {
            char c = buf[i];
            bytes++;
            if (c == '\n')
                lines++;
            int space = c == ' ' || c == '\n' || c == '\t';
            if (!space && !in_word)
                words++;
            in_word = !space;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc == 1)
        count(0);
    for (int i = 1; i < argc; i++) {
        int32_t fd = open(argv[i], 0);
        if (fd < 0) {
            eprint("wc: ");
            eprint(argv[i]);
            eprint(": cannot open\n");
            return 1;
        }
        count(fd);
        close(fd);
    }
    print_num(lines);
    print(" ");
    print_num(words);
    print(" ");
    print_num(bytes);
    print("\n");
    return 0;
}
