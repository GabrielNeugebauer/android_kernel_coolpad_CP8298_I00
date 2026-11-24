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

static void me1ds_con_write(struct console *con, const char *s, unsigned n)
{
	const u8 *glyphs = font_vga_8x16.data;
	int b, y;

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
	me1ds_text_on = true;
	register_console(&me1ds_con);
	return 0;
}
console_initcall(me1ds_con_init);
