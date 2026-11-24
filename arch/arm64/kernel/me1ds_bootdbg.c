#include <linux/io.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <asm/cacheflush.h>
#include <asm/me1ds_bootdbg.h>

static void *fb;
static bool linear;

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

	if (me1ds_fb_map())
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

	if (me1ds_fb_map())
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
