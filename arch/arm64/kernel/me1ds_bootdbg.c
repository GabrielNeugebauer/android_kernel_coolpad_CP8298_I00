#include <linux/console.h>
#include <linux/font.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <asm/cacheflush.h>
#include <asm/me1ds_bootdbg.h>

static void *fb;
static bool linear;
static bool me1ds_text_on;

static int me1ds_fb_map(void)
{
	if (!fb) {
		linear = pfn_valid(__phys_to_pfn(ME1DS_FB_BASE));
		if (linear)
			fb = phys_to_virt(ME1DS_FB_BASE);
		else
			fb = (void __force *)ioremap_wc(ME1DS_FB_BASE,
					ME1DS_FB_BUF_SIZE * ME1DS_FB_NR_BUF);
	}
	return fb ? 0 : -ENOMEM;
}

void me1ds_boot_stripe(int n)
{
	int b;

	if (me1ds_text_on || me1ds_fb_map())
		return;

	for (b = 0; b < ME1DS_FB_NR_BUF; b++) {
		void *p = fb + b * ME1DS_FB_BUF_SIZE + n * ME1DS_STRIPE_PITCH;

		if (linear) {
			memset(p, 0xff, ME1DS_STRIPE_SIZE);
			__flush_dcache_area(p, ME1DS_STRIPE_SIZE);
		} else {
			memset_io((void __iomem *)p, 0xff, ME1DS_STRIPE_SIZE);
		}
	}
}

/* Black 64-pixel notch at the left end of stripe n (initcall level n done). */
void me1ds_boot_notch(int n)
{
	int b, y;

	if (me1ds_text_on || me1ds_fb_map())
		return;

	for (b = 0; b < ME1DS_FB_NR_BUF; b++)
		for (y = 0; y < 85; y++) {
			void *p = fb + b * ME1DS_FB_BUF_SIZE + n * ME1DS_STRIPE_PITCH +
				  y * ME1DS_FB_STRIDE;

			if (linear) {
				memset(p, 0, 64 * 4);
				__flush_dcache_area(p, 64 * 4);
			} else {
				memset_io((void __iomem *)p, 0, 64 * 4);
			}
		}
}

/*
 * Text console on the lk framebuffer: 60x53 cells of 8x16, wrapping from
 * the top; a white bar marks the line after the newest one. Every
 * initcall is logged, so the last line names the one that hangs.
 */
#define COLS	(480 / 8)
#define ROWS	(854 / 16)

extern bool initcall_debug;
static int cur_x, cur_y;

static void me1ds_put_px_row(int b, int y, int x, u8 bits)
{
	u32 __iomem *p = (u32 __iomem *)(fb + b * ME1DS_FB_BUF_SIZE +
					 y * ME1DS_FB_STRIDE + x * 4);
	int i;

	for (i = 0; i < 8; i++)
		writel_relaxed((bits & (0x80 >> i)) ? 0xffffffff : 0, p + i);
}

static void me1ds_fill_row(int row, u32 val)
{
	int b, y, x;

	for (b = 0; b < ME1DS_FB_NR_BUF; b++)
		for (y = row * 16; y < row * 16 + 16; y++)
			for (x = 0; x < 480; x++)
				writel_relaxed(val, (u32 __iomem *)(fb + b * ME1DS_FB_BUF_SIZE +
						y * ME1DS_FB_STRIDE) + x);
}

static void me1ds_newline(void)
{
	cur_x = 0;
	cur_y = (cur_y + 1) % ROWS;
	me1ds_fill_row(cur_y, 0);
	me1ds_fill_row((cur_y + 1) % ROWS, 0xffffffff);
}

/*
 * Once an oops starts, keep only the lines that identify it (no register
 * or stack dumps, no other CPUs' backtraces) and freeze at the panic line
 * so the report stays on screen until the reset.
 */
static bool me1ds_oops, me1ds_frozen;

static bool me1ds_has(const char *s, unsigned n, const char *pat)
{
	return strnstr(s, pat, n) != NULL;
}

static bool me1ds_keep_line(const char *s, unsigned n)
{
	static const char * const starts[] = {
		"Unable to handle", "Internal error", "kernel BUG",
		"Kernel panic", "Bad mode", "BUG:", "Oops",
	};
	static const char * const keep[] = {
		"PC is at", "LR is at", "Call trace", "[<ffffff", "Comm:",
		"Process ", "pgd =", "Code:", "Unable to handle",
		"Internal error", "kernel BUG", "Kernel panic", "Bad mode",
		"BUG:", "Oops", "Exception stack",
	};
	int i;

	if (me1ds_frozen)
		return false;
	if (!me1ds_oops) {
		for (i = 0; i < ARRAY_SIZE(starts); i++)
			if (me1ds_has(s, n, starts[i]))
				me1ds_oops = true;
		if (!me1ds_oops)
			return true;
	}
	for (i = 0; i < ARRAY_SIZE(keep); i++)
		if (me1ds_has(s, n, keep[i])) {
			if (me1ds_has(s, n, "Kernel panic"))
				me1ds_frozen = true;
			return true;
		}
	return false;
}

/*
 * Every line also goes into the ram_console buffer at 0x43f00000 in the
 * layout LG's stock 3.10 kernel expects (preloader header kept, console
 * ring after the 32-bit last_reboot_reason), so stock Android shows this
 * log in /proc/last_kmsg after a watchdog reset.
 */
#define ME1DS_RAMLOG_BASE	0x43f00000
#define ME1DS_RAMLOG_SIZE	0x10000
#define ME1DS_RC_SIG		0x43474244	/* "DBGC" */
#define ME1DS_LRR_SIZE		0x180	/* ALIGN(sizeof(last_reboot_reason) on 32-bit, 64) */

struct me1ds_rc_hdr {
	u32 sig;
	u32 off_pl, off_lpl, sz_pl;
	u32 off_lk, off_llk, sz_lk;
	u32 padding[3];
	u32 sz_buffer;
	u32 off_linux;
	u32 off_console;
	u32 log_start;
	u32 log_size;
	u32 sz_console;
};

static struct me1ds_rc_hdr *me1ds_rc;
static char *me1ds_rc_log;

static void me1ds_ram_init(void)
{
	struct me1ds_rc_hdr *h = phys_to_virt(ME1DS_RAMLOG_BASE);

	if (h->sig != ME1DS_RC_SIG || h->sz_buffer != ME1DS_RAMLOG_SIZE) {
		memset(h, 0, ME1DS_RAMLOG_SIZE);
		h->sig = ME1DS_RC_SIG;
		h->sz_buffer = ME1DS_RAMLOG_SIZE;
	}
	if (h->sz_lk && h->off_lk + ALIGN(h->sz_lk, 64) == h->off_llk)
		h->off_linux = h->off_llk + ALIGN(h->sz_lk, 64);
	else
		h->off_linux = 512;
	h->off_console = h->off_linux + ME1DS_LRR_SIZE;
	h->sz_console = h->sz_buffer - h->off_console;
	h->log_start = 0;
	h->log_size = 0;
	memset((char *)h + h->off_linux, 0, h->sz_buffer - h->off_linux);
	__flush_dcache_area(h, ME1DS_RAMLOG_SIZE);
	me1ds_rc_log = (char *)h + h->off_console;
	me1ds_rc = h;
}

static void me1ds_ram_put(const char *s, unsigned n)
{
	struct me1ds_rc_hdr *h = me1ds_rc;
	u32 size = h->sz_console, start = h->log_start, rem;

	if (n >= size) {
		memcpy(me1ds_rc_log, s + n - size, size);
		h->log_start = 0;
		h->log_size = size;
		__flush_dcache_area(me1ds_rc_log, size);
	} else if (n > size - start) {
		rem = size - start;
		memcpy(me1ds_rc_log + start, s, rem);
		memcpy(me1ds_rc_log, s + rem, n - rem);
		h->log_start = n - rem;
		h->log_size = size;
		__flush_dcache_area(me1ds_rc_log + start, rem);
		__flush_dcache_area(me1ds_rc_log, n - rem);
	} else {
		memcpy(me1ds_rc_log + start, s, n);
		h->log_start += n;
		h->log_size += n;
		if (h->log_start >= size)
			h->log_start = 0;
		if (h->log_size > size)
			h->log_size = size;
		__flush_dcache_area(me1ds_rc_log + start, n);
	}
	__flush_dcache_area(h, sizeof(*h));
}

static void me1ds_con_write(struct console *con, const char *s, unsigned n)
{
	const u8 *glyphs = font_vga_8x16.data;
	int b, y;

	if (me1ds_rc)
		me1ds_ram_put(s, n);

	if (!me1ds_keep_line(s, n))
		return;

	for (; n; n--, s++) {
		if (*s == '\n') {
			me1ds_newline();
			continue;
		}
		if (cur_x >= COLS)
			me1ds_newline();
		for (b = 0; b < ME1DS_FB_NR_BUF; b++)
			for (y = 0; y < 16; y++)
				me1ds_put_px_row(b, cur_y * 16 + y, cur_x * 8,
						 glyphs[(u8)*s * 16 + y]);
		cur_x++;
	}
	wmb();
}

static struct console me1ds_con = {
	.name	= "me1dsfb",
	.write	= me1ds_con_write,
	.flags	= CON_PRINTBUFFER | CON_ENABLED | CON_ANYTIME,
	.index	= -1,
};

static int __init me1ds_con_init(void)
{
	/* the lk framebuffer sits above the RAM given to Linux */
	if (me1ds_fb_map() || linear)
		return 0;
	me1ds_fill_row(0, 0);
	me1ds_fill_row(1, 0xffffffff);
	initcall_debug = true;
	console_loglevel = 8;
	if (pfn_valid(__phys_to_pfn(ME1DS_RAMLOG_BASE))) {
		me1ds_ram_init();
		me1ds_ram_put("== me1ds 3.18 boot log ==\n", 26);
	}
	me1ds_text_on = true;
	register_console(&me1ds_con);
	return 0;
}
console_initcall(me1ds_con_init);
