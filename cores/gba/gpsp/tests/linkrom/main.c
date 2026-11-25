/* The link test ROM: talks to a GBA Wireless Adapter (as gpSP emulates it, gpsp_serial=rfu) to check that two
 * emulators reach each other over pixelado-core-gpsp's link. Byte 0 of the save says the role ('H' host, 'C' guest).
 *   host:  init, set its broadcast data ("PXL1"...), start hosting, then poll for a connected guest
 *   guest: init, read broadcasts until the host's "PXL1" shows up, connect to it, poll until connected
 * Then byte 1 of the save is 'K' (connected) or 'X' (gave up after 20 s), byte 2 the last step reached, and the
 * screen is green or red (blue while trying). Each step needs packets in both directions through the link: the
 * host's broadcast (host -> guest), the connection request (guest -> host) and its answer (host -> guest).
 * No libc, no interrupts, no data or bss sections. GPL-3.0-or-later like the rest of this repository. */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define REG16(a) (*(volatile u16 *)(a))
#define REG32(a) (*(volatile u32 *)(a))
#define DISPCNT REG16(0x04000000)
#define VCOUNT REG16(0x04000006)
#define SIODATA32 REG32(0x04000120)
#define SIOCNT REG16(0x04000128)
#define RCNT REG16(0x04000134)
#define VRAM ((volatile u16 *)0x06000000)
#define SRAM ((volatile u8 *)0x0E000000)

#define BLUE 0x7C00
#define GREEN 0x03E0
#define RED 0x001F
#define GREY 0x4210
#define MAGIC 0x314C5850u   /* "PXL1" */

static void vsync(void)
{
    while (VCOUNT >= 160) {}
    while (VCOUNT < 160) {}
}

static void band(u16 col, int y0, int y1)
{
    int x, y;
    for (y = y0; y < y1; y++)
        for (x = 0; x < 240; x++) VRAM[y * 240 + x] = col;
}

/* one 32-bit word each way with the adapter (normal mode, internal clock, 2 MHz) */
static u32 xfer(u32 v)
{
    SIODATA32 = v;
    SIOCNT = 0x1003;
    SIOCNT = 0x1083;
    while (SIOCNT & 0x80) {}
    return SIODATA32;
}

/* an adapter command; its answer's words into out. -1: no answer, -2: the adapter's error */
static int cmd(u32 c, const u32 *args, int n, u32 *out, int max)
{
    int i, len;
    u32 r;
    xfer(0x99660000u | ((u32)n << 8) | c);
    for (i = 0; i < n; i++) xfer(args[i]);
    r = xfer(0x80000000u);
    if ((r >> 16) != 0x9966) return -1;
    if ((r & 0xFF) == 0xEE) {
        xfer(0x80000000u);
        return -2;
    }
    len = (int)((r >> 8) & 0xFF);
    for (i = 0; i < len; i++) {
        const u32 w = xfer(0x80000000u);
        if (i < max) out[i] = w;
    }
    return len;
}

static void step(int s)
{
    SRAM[2] = (u8)s;
    band(GREY, 150 - 0, 160);
    band(BLUE, 150, 150 + (s > 10 ? 10 : s));
}

static void done(int ok)
{
    SRAM[1] = ok ? 'K' : 'X';
    band(ok ? GREEN : RED, 0, 150);
    for (;;) vsync();
}

int main(void)
{
    u32 out[32], arg[6];
    int i, n, t;
    const int host = SRAM[0] == 'H';
    DISPCNT = 0x0403;   /* mode 3, BG2 */
    band(BLUE, 0, 150);
    band(GREY, 150, 160);
    /* the adapter's reset (SD pulled high in GPIO mode), then normal 32-bit mode */
    RCNT = 0x8000;
    RCNT = 0x80A0;
    RCNT = 0x0000;
    SIOCNT = 0x1003;
    /* Nintendo's handshake, as the adapter checks it */
    xfer(0x0000494E);
    xfer(0xB0BB8001);
    step(1);
    if (cmd(0x10, 0, 0, out, 32) < 0) done(0);
    step(2);
    if (host) {
        arg[0] = MAGIC;
        for (i = 1; i < 6; i++) arg[i] = 0x01010101u * (u32)i;
        if (cmd(0x16, arg, 6, out, 32) < 0) done(0);   /* the broadcast data */
        if (cmd(0x19, 0, 0, out, 32) < 0) done(0);     /* host: start */
        step(3);
        for (t = 0; t < 20 * 60; t++) {
            vsync();
            n = cmd(0x1A, 0, 0, out, 32);              /* host: who is connected */
            if (n > 0) {
                step(4);
                done(1);
            }
        }
        done(0);
    } else {
        u32 dev = 0;
        if (cmd(0x1C, 0, 0, out, 32) < 0) done(0);     /* broadcast read: start */
        step(3);
        for (t = 0; t < 20 * 60 && !dev; t++) {
            vsync();
            n = cmd(0x1D, 0, 0, out, 32);              /* broadcast read: the hosts heard (7 words each) */
            for (i = 0; i + 7 <= n && i + 7 <= 32; i += 7)
                if (out[i + 1] == MAGIC) dev = out[i] & 0xFFFF;
        }
        if (!dev) done(0);
        step(4);
        cmd(0x1E, 0, 0, out, 32);                      /* broadcast read: stop */
        arg[0] = dev;
        if (cmd(0x1F, arg, 1, out, 32) < 0) done(0);   /* connect to the host */
        step(5);
        for (; t < 20 * 60; t++) {
            vsync();
            n = cmd(0x20, 0, 0, out, 32);              /* connected yet? */
            if (n >= 1 && out[0] != 0x01000000u) {
                if (out[0] == 0x02000000u) done(0);
                step(6);
                done(1);
            }
        }
        done(0);
    }
    return 0;
}

/* (in case the compiler calls them) */
void *memset(void *d, int c, unsigned long n)
{
    u8 *p = d;
    while (n--) *p++ = (u8)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n)
{
    u8 *p = d;
    const u8 *q = s;
    while (n--) *p++ = *q++;
    return d;
}
