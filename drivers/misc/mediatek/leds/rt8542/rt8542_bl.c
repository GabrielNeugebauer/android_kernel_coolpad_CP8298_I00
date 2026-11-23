/*
 * RT8542 backlight charge pump on the LG K130 (me1ds).
 *
 * Register sequence and brightness table from LG's rt8542_driver.c
 * (LG K130 kernel source); reduced to the backlight path for the
 * 3.18 MediaTek LED framework. Flash control is not handled here.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <mt-plat/mt_gpio.h>

#define RT8542_GPIO_BL_EN	(GPIO65 | 0x80000000)
#define RT8542_REG_OVP		0x02
#define RT8542_REG_BL		0x05
#define RT8542_REG_EN		0x0A
#define RT8542_MIN_LEVEL	10
#define RT8542_MAX_LEVEL	255

static const u8 bright_arr[] = {
	1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4,
	4, 4, 4, 5, 5, 6, 6, 6, 7, 7, 8, 8, 8, 9, 9, 10, 10, 10, 11, 11,
	12, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 21, 22, 23, 24, 25, 26, 27, 28, 29,
	30, 31, 33, 34, 35, 37, 38, 39, 40, 42, 43, 44, 46, 47, 48, 50, 51, 52, 54, 55,
	57, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84, 86, 88, 90, 92, 94, 96};

static struct i2c_client *rt8542_client;
static DEFINE_MUTEX(rt8542_lock);
static bool rt8542_on;

static int rt8542_write(u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(rt8542_client, reg, val);
}

static int rt8542_read(u8 reg)
{
	return i2c_smbus_read_byte_data(rt8542_client, reg);
}

static void rt8542_set_en(u8 set, u8 mask)
{
	int v = rt8542_read(RT8542_REG_EN);

	if (v < 0)
		return;
	rt8542_write(RT8542_REG_EN, (v | set) & mask);
}

int rt8542_set_backlight(int level, int div)
{
	unsigned int idx;

	if (level < 0)
		level = 0;

	if (!rt8542_client)
		return -ENODEV;

	mutex_lock(&rt8542_lock);
	if (level == 0) {
		rt8542_write(RT8542_REG_BL, 0);
		rt8542_set_en(0, 0x66);
		mt_set_gpio_out(RT8542_GPIO_BL_EN, GPIO_OUT_ZERO);
		rt8542_on = false;
		goto out;
	}

	level = clamp_t(int, level, RT8542_MIN_LEVEL, RT8542_MAX_LEVEL);
	idx = (level - RT8542_MIN_LEVEL) * 100 / (RT8542_MAX_LEVEL - RT8542_MIN_LEVEL);

	if (!rt8542_on) {
		mt_set_gpio_out(RT8542_GPIO_BL_EN, GPIO_OUT_ONE);
		mdelay(10);
		rt8542_write(RT8542_REG_OVP, 0x54);
		rt8542_write(RT8542_REG_BL, bright_arr[idx]);
		rt8542_set_en(0x19, 0x99);
		rt8542_on = true;
	}
	rt8542_write(RT8542_REG_BL, bright_arr[idx]);
out:
	mutex_unlock(&rt8542_lock);
	return 0;
}

static int rt8542_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	mt_set_gpio_mode(RT8542_GPIO_BL_EN, GPIO_MODE_00);
	mt_set_gpio_dir(RT8542_GPIO_BL_EN, GPIO_DIR_OUT);
	/* lk leaves the backlight running */
	mt_set_gpio_out(RT8542_GPIO_BL_EN, GPIO_OUT_ONE);
	rt8542_on = true;
	rt8542_client = client;
	dev_info(&client->dev, "rt8542 backlight ready\n");
	return 0;
}

static const struct i2c_device_id rt8542_id[] = {
	{ "rt8542", 0 },
	{ }
};

static const struct of_device_id rt8542_of_match[] = {
	{ .compatible = "mediatek,i2c_lcd_bias" },
	{ .compatible = "richtek,rt8542" },
	{ }
};

static struct i2c_driver rt8542_driver = {
	.probe = rt8542_probe,
	.id_table = rt8542_id,
	.driver = {
		.name = "rt8542",
		.of_match_table = rt8542_of_match,
	},
};

static int __init rt8542_init(void)
{
	return i2c_add_driver(&rt8542_driver);
}
subsys_initcall(rt8542_init);
MODULE_LICENSE("GPL");
