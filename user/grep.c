/* grep 文字 [文件…]：打印含有这段文字的行。没有给文件就读标准输入。 */
#include "ulib.h"

#define LINE_LEN 256

static int contains(const char *line, const char *word)
{
    for (; *line; line++) {
        int i = 0;
        while (word[i] && line[i] == word[i])
            i++;
        if (word[i] == '\0')
            return 1;
    }
    return 0;
}

static int found;

static void grep(int32_t fd, const char *word)
{
    char buf[512], line[LINE_LEN];
    uint32_t len = 0;
    int32_t n;

    for (;;) {
        n = read(fd, buf, sizeof(buf));
        for (int32_t i = 0; i < n; i++) {
            if (buf[i] != '\n' && len < LINE_LEN - 1) {
                line[len++] = buf[i];
                continue;
            }
            if (buf[i] != '\n')
                continue;           /* 太长的行截断 */
            line[len] = '\0';
            if (contains(line, word)) {
                found = 1;
                print(line);
                print("\n");
            }
            len = 0;
        }
        if (n <= 0)
            break;
    }
    if (len > 0) {                  /* 最后一行没有换行 */
        line[len] = '\0';
        if (contains(line, word)) {
            found = 1;
            print(line);
            print("\n");
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        eprint("usage: grep text [file...]\n");
        return 2;
    }
    if (argc == 2)
        grep(0, argv[1]);
    for (int i = 2; i < argc; i++) {
        int32_t fd = open(argv[i], 0);
        if (fd < 0) {
            eprint("grep: ");
            eprint(argv[i]);
            eprint(": cannot open\n");
            continue;
        }
        grep(fd, argv[1]);
        close(fd);
    }
    return found ? 0 : 1;
}
