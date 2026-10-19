/*
 * sort [-r] [文件…]：把所有的行读进内存，排好序再打印；没给文件就读标准输入。-r 倒过来排。
 * 事先不知道有多少行、每行多长，所以内容和每一行的位置都放在 malloc 来的堆上，不够了就用 realloc 扩到两倍：
 * 在堆末尾的块原地变大，不用搬。按字节比较（0～255），行里可以有任何字节，打印时每行后面都加一个换行。
 * 一个文件的最后一行没有换行，也算一行，不会和下一个文件的第一行接在一起。
 */
#include "ulib.h"

struct line {
    const char *s;
    uint32_t n;
};

static char *text;
static uint32_t text_len, text_cap;
static struct line *lines;
static uint32_t nlines, lines_cap;
static int reverse;

/* 把 *buf 扩成至少能放 need 个 elem 字节的元素：每次翻倍。 */
static int grow(void **buf, uint32_t *cap, uint32_t need, uint32_t elem)
{
    if (need <= *cap)
        return 1;
    uint32_t n = *cap ? *cap : 256;
    while (n < need)
        n *= 2;
    void *p = realloc(*buf, n * elem);
    if (!p)
        return 0;
    *buf = p;
    *cap = n;
    return 1;
}

/* 读完一个文件：1 表示读完了，0 表示内存不够，-1 表示读出错。 */
static int read_all(int32_t fd)
{
    for (;;) {
        if (!grow((void **)&text, &text_cap, text_len + 512, 1))
            return 0;
        int32_t n = read(fd, text + text_len, 512);
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        text_len += (uint32_t)n;
    }
    if (text_len && text[text_len - 1] != '\n')     /* 最后一行没有换行：补一个，免得和下一个文件接成一行 */
        text[text_len++] = '\n';                     /* grow 时总留着 512 字节，放得下 */
    return 1;
}

static int less(const struct line *a, const struct line *b)
{
    uint32_t n = a->n < b->n ? a->n : b->n;
    for (uint32_t i = 0; i < n; i++)
        if (a->s[i] != b->s[i])
            return ((unsigned char)a->s[i] < (unsigned char)b->s[i]) != reverse;
    return (a->n < b->n) != reverse && a->n != b->n;
}

/* 归并排序：把 [lo, hi) 排好，tmp 是同样大小的临时空间。 */
static void merge_sort(struct line *v, struct line *tmp, uint32_t lo, uint32_t hi)
{
    if (hi - lo < 2)
        return;
    uint32_t mid = lo + (hi - lo) / 2, i = lo, j = mid, k = lo;
    merge_sort(v, tmp, lo, mid);
    merge_sort(v, tmp, mid, hi);
    while (i < mid || j < hi)
        tmp[k++] = j >= hi || (i < mid && !less(&v[j], &v[i])) ? v[i++] : v[j++];
    for (k = lo; k < hi; k++)
        v[k] = tmp[k];
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        reverse = 1;
        first = 2;
    }
    int ok = 1;
    if (first == argc)
        ok = read_all(0);
    for (int i = first; i < argc && ok > 0; i++) {
        int32_t fd = open(argv[i], 0);
        if (fd < 0) {
            eprint("sort: ");
            eprint(argv[i]);
            eprint(": cannot open\n");
            return 1;
        }
        ok = read_all(fd);
        close(fd);
    }
    for (uint32_t start = 0; ok > 0 && start < text_len;) {
        uint32_t end = start;
        while (end < text_len && text[end] != '\n')
            end++;
        if ((ok = grow((void **)&lines, &lines_cap, nlines + 1, sizeof(struct line))))
            lines[nlines++] = (struct line){ text + start, end - start };
        start = end + 1;
    }
    struct line *tmp = ok > 0 ? malloc(nlines * sizeof(struct line)) : 0;
    if (!tmp) {
        eprint(ok < 0 ? "sort: read error\n" : "sort: out of memory\n");
        return 1;
    }
    merge_sort(lines, tmp, 0, nlines);
    for (uint32_t i = 0; i < nlines; i++) {
        write(1, lines[i].s, lines[i].n);
        write(1, "\n", 1);
    }
    return 0;
}
