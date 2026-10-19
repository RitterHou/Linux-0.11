/*
 * 硬盘：主 IDE 通道上的主盘（hda）和从盘（hdb），ATA 接口，端口 0x1F0～0x1F7，中断 IRQ14。
 * 两块盘共用一个通道：选盘寄存器的第 4 位选主盘还是从盘，一次只能有一个请求。用 PIO 方式一次读写一个 512 字节的扇区。
 *
 * 读一个扇区：选盘并写好 LBA 和扇区数，发 READ SECTORS 命令，然后睡在等待队列上，CPU 去运行别的任务。
 * 硬盘把数据准备好后拉 IRQ14，hd_handle() 从数据端口读走 256 个字，再叫醒等着的任务。
 * 写一个扇区：发 WRITE SECTORS 命令，等硬盘要数据（DRQ），写进 256 个字，睡下等它写完后的 IRQ14。
 * Linux 0.11 的 kernel/blk_drv/hd.c 也是这样：hd_out() 发命令，read_intr() / write_intr() 在中断里搬数据，
 * end_request() 唤醒等待的进程。它还有请求队列、用 CHS 寻址；这里一次只处理一个请求，用 LBA 寻址。
 */
#include "io.h"
#include "kernel.h"

#define HD_DATA     0x1F0           /* 数据端口，16 位 */
#define HD_NSECTOR  0x1F2           /* 扇区数 */
#define HD_LBA0     0x1F3           /* LBA 的第 0～7 位 */
#define HD_LBA1     0x1F4           /*        第 8～15 位 */
#define HD_LBA2     0x1F5           /*        第 16～23 位 */
#define HD_DRIVE    0x1F6           /* 选盘：0xE0 | 盘号 << 4 | LBA 的第 24～27 位：LBA 寻址，盘号 0 是主盘、1 是从盘 */
#define HD_STATUS   0x1F7           /* 读：状态 */
#define HD_COMMAND  0x1F7           /* 写：命令 */
#define HD_CONTROL  0x3F6           /* 设备控制：第 1 位 nIEN 为 1 时硬盘不发中断 */

#define ST_ERR      0x01            /* 出错 */
#define ST_DRQ      0x08            /* 数据准备好了，可以读写数据端口 */
#define ST_DF       0x20            /* 设备故障 */
#define ST_BSY      0x80            /* 忙：其余各位都无效 */

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_IDENTIFY 0xEC

#define DISK_ROW    1               /* 第 1 行显示主盘的大小和型号 */

uint32_t hd_sectors[2];             /* 主盘、从盘的扇区数，0 表示没有这块盘 */
uint32_t hd_requests;               /* 读写过多少个扇区 */

static struct wait_queue hd_wait;   /* 等硬盘空出来、或者等这次读写完成的任务 */
static int busy;                    /* 有一个请求正在进行 */
static volatile int done, failed;
static uint16_t *buffer;            /* 这次请求的数据放在哪里 */
static int writing;

/* 轮询，等硬盘不忙。只在发命令之前用，正常情况下硬盘早就空闲了。 */
static uint8_t wait_ready(void)
{
    uint8_t st = 0;
    for (int i = 0; i < 1000000; i++) {
        st = inb(HD_STATUS);
        if (!(st & ST_BSY))
            break;
    }
    return st;
}

/*
 * 用 IDENTIFY 命令问一块盘：返回扇区数，id 里是它回答的 256 个字；没有这块盘返回 0，*why 说明原因。
 * 选了盘以后先读几次状态（约 400 ns），ATA 规定换盘以后要等这么久，状态才是新选的那块盘的。
 */
static uint32_t identify(int drive, uint16_t *id, const char **why)
{
    outb(HD_DRIVE, 0xA0 | drive << 4);
    for (int i = 0; i < 4; i++)
        inb(HD_STATUS);
    uint8_t st = inb(HD_STATUS);
    if (st == 0xFF || st == 0x00) { /* 0xFF：这个通道上没有设备；0：没有这块盘 */
        *why = "none";
        return 0;
    }
    outb(HD_COMMAND, CMD_IDENTIFY);
    do
        st = wait_ready();
    while (!(st & (ST_DRQ | ST_ERR)));
    if (st & ST_ERR) {
        *why = "not ATA";
        return 0;
    }
    insw(HD_DATA, id, 256);
    return id[60] | (uint32_t)id[61] << 16;     /* 第 60、61 个字：LBA28 下的总扇区数 */
}

/*
 * 开机时调用：问出主盘、从盘的扇区数，主盘的大小和型号显示在第 1 行，再打开 IRQ14。
 * 此时还是任务 0 在运行，不能睡，所以先关掉硬盘的中断，用轮询等它回答。
 */
void hd_init(void)
{
    uint16_t id[256];
    struct text_line line;
    const char *why = "";

    line_init(&line);
    outb(HD_CONTROL, 0x02);
    hd_sectors[1] = identify(1, id, &why);      /* 先问从盘：最后选着的要是主盘，和原来一样 */
    hd_sectors[0] = identify(0, id, &why);
    if (hd_sectors[0] == 0) {
        line_append(&line, "disk: ");
        line_append(&line, why);
        line_show(&line, VGA_GRAY, DISK_ROW);
        return;
    }

    /* 第 27～46 个字是型号，每个字里两个字符的顺序是反的，末尾补的是空格 */
    char model[41];
    for (int i = 0; i < 20; i++) {
        model[2 * i] = (char)(id[27 + i] >> 8);
        model[2 * i + 1] = (char)(id[27 + i] & 0xFF);
    }
    int len = 40;
    while (len > 0 && model[len - 1] == ' ')
        len--;
    model[len] = '\0';

    line_append(&line, "disk: ");
    line_append_dec(&line, hd_sectors[0]);
    line_append(&line, " sectors (");
    line_append_dec(&line, hd_sectors[0] / 2);
    line_append(&line, " KB), ");
    line_append(&line, model);
    line_show(&line, VGA_GRAY, DISK_ROW);

    outb(HD_CONTROL, 0x00);         /* 打开硬盘的中断 */
    pic_unmask(14);
}

/* 读写 drive 号盘的一个扇区，成功返回 0。调用者必须是能睡的任务，不能是任务 0。 */
static int hd_request(int drive, uint32_t lba, void *buf, int write)
{
    if (drive < 0 || drive > 1 || lba >= hd_sectors[drive])
        return -1;
    uint32_t flags = irq_save();
    while (busy)                    /* 别的任务正在用硬盘：排队 */
        sleep_on(&hd_wait);
    busy = 1;
    done = failed = 0;
    buffer = buf;
    writing = write;

    wait_ready();
    outb(HD_DRIVE, 0xE0 | drive << 4 | ((lba >> 24) & 0x0F));
    outb(HD_NSECTOR, 1);
    outb(HD_LBA0, lba & 0xFF);
    outb(HD_LBA1, (lba >> 8) & 0xFF);
    outb(HD_LBA2, (lba >> 16) & 0xFF);
    outb(HD_COMMAND, write ? CMD_WRITE : CMD_READ);
    if (write) {
        /* 写：硬盘先要数据，不发中断。等它准备好就把一个扇区写进去，写完它再发 IRQ14。 */
        uint8_t st;
        do
            st = wait_ready();
        while (!(st & (ST_DRQ | ST_ERR | ST_DF)));
        if (st & ST_DRQ)
            outsw(HD_DATA, buf, 256);
        else
            failed = done = 1;
    }
    while (!done)
        sleep_on(&hd_wait);
    int ok = !failed;
    busy = 0;
    hd_requests++;
    wake_up(&hd_wait);              /* 让排队的任务接着用 */
    irq_restore(flags);
    return ok ? 0 : -1;
}

int hd_read(int drive, uint32_t lba, void *buf)
{
    return hd_request(drive, lba, buf, 0);
}

int hd_write(int drive, uint32_t lba, const void *buf)
{
    return hd_request(drive, lba, (void *)buf, 1);
}

/* IRQ14：读状态寄存器（同时也告诉硬盘中断收到了），读请求就把数据读走，然后叫醒等着的任务。 */
void hd_handle(void)
{
    uint8_t st = inb(HD_STATUS);

    if (!busy || done)              /* 没有请求时来的中断，不理它 */
        return;
    if (st & (ST_ERR | ST_DF))
        failed = 1;
    else if (!writing && (st & ST_DRQ))
        insw(HD_DATA, buffer, 256);
    else if (!writing)
        failed = 1;
    done = 1;
    wake_up(&hd_wait);
}
