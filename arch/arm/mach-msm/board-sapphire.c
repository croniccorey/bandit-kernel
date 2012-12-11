/* linux/arch/arm/mach-msm/board-sapphire.c
 * Copyright (C) 2007-2009 HTC Corporation.
 * Author: Thomas Tsai <thomas_tsai@htc.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
*/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/i2c-msm.h>
#include <linux/irq.h>
#include <linux/keyreset.h>
#include <linux/leds.h>
#include <linux/switch.h>
#include <linux/synaptics_i2c_rmi.h>
#include <linux/elan_i2c.h>
#include <linux/akm8976.h>
#include <linux/sysdev.h>

#include <linux/delay.h>
#include <linux/gpio.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/flash.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/setup.h>

#include <linux/gpio_event.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>
#include <linux/mmc/sdio_ids.h>

#include <mach/system.h>
#include <mach/gpio_chip.h>
#include <mach/vreg.h>
#include "board-sapphire.h"

#include <mach/hardware.h>
#include <mach/board_htc.h>
#include <mach/msm_serial_hs.h>
#include <mach/msm_serial_debugger.h>
#include <mach/msm_fb.h>
#include <mach/h2w_v1.h>
#include <mach/microp_i2c.h>
#include <mach/htc_battery.h>
#include <mach/htc_pwrsink.h>
#include <mach/perflock.h>
#include <mach/drv_callback.h>


#include "proc_comm.h"
#include "devices.h"

void msm_init_irq(void);
void msm_init_gpio(void);
void msm_init_pmic_vibrator(void);

extern int sapphire_init_mmc(unsigned int);

struct sapphire_axis_info {
	struct gpio_event_axis_info info;
	uint16_t in_state;
	uint16_t out_state;
	uint16_t temp_state;
	uint16_t threshold;
};
static bool nav_just_on;
static int nav_on_jiffies;
static unsigned int hwid = 0;
static unsigned int skuid = 0;
static unsigned engineerid = (0x01 << 1);	/* default is 3M sensor */
#if	defined(CONFIG_MSM_AMSS_SUPPORT_256MB_EBI1)
static int smi_sz = 32;
#else
static int smi_sz = 64;
#endif
uint16_t sapphire_axis_map(struct gpio_event_axis_info *info, uint16_t in)
{
	struct sapphire_axis_info *ai = container_of(info, struct sapphire_axis_info, info);
	uint16_t out = ai->out_state;

	if (nav_just_on) {
		if (jiffies == nav_on_jiffies || jiffies == nav_on_jiffies + 1)
			goto ignore;
		nav_just_on = 0;
	}
	if ((ai->in_state ^ in) & 1)
		out--;
	if ((ai->in_state ^ in) & 2)
		out++;
	ai->out_state = out;
ignore:
	ai->in_state = in;
	if (ai->out_state - ai->temp_state == ai->threshold) {
		ai->temp_state++;
		ai->out_state = ai->temp_state;
	} else if (ai->temp_state - ai->out_state == ai->threshold) {
		ai->temp_state--;
		ai->out_state = ai->temp_state;
	} else if (abs(ai->out_state - ai->temp_state) > ai->threshold)
		ai->temp_state = ai->out_state;

	return ai->temp_state;
}

int sapphire_nav_power(const struct gpio_event_platform_data *pdata, bool on)
{
	gpio_set_value(SAPPHIRE_GPIO_JOG_EN, on);
	if (on) {
		nav_just_on = 1;
		nav_on_jiffies = jiffies;
	}
	return 0;
}

static uint32_t sapphire_x_axis_gpios[] = {
	SAPPHIRE_BALL_LEFT_0, SAPPHIRE_BALL_RIGHT_0
};

static struct sapphire_axis_info sapphire_x_axis = {
	.threshold = 2,
	.info = {
		.info.func = gpio_event_axis_func,
		.count = ARRAY_SIZE(sapphire_x_axis_gpios),
		.type = EV_REL,
		.code = REL_X,
		.decoded_size = 1U << ARRAY_SIZE(sapphire_x_axis_gpios),
		.map = sapphire_axis_map,
		.gpio = sapphire_x_axis_gpios,
		.flags = GPIOEAF_PRINT_UNKNOWN_DIRECTION /*| GPIOEAF_PRINT_RAW | GPIOEAF_PRINT_EVENT */
	}
};

static uint32_t sapphire_y_axis_gpios[] = {
	SAPPHIRE_BALL_UP_0, SAPPHIRE_BALL_DOWN_0
};

static struct sapphire_axis_info sapphire_y_axis = {
	.threshold = 2,
	.info = {
		.info.func = gpio_event_axis_func,
		.count = ARRAY_SIZE(sapphire_y_axis_gpios),
		.type = EV_REL,
		.code = REL_Y,
		.decoded_size = 1U << ARRAY_SIZE(sapphire_y_axis_gpios),
		.map = sapphire_axis_map,
		.gpio = sapphire_y_axis_gpios,
		.flags = GPIOEAF_PRINT_UNKNOWN_DIRECTION /*| GPIOEAF_PRINT_RAW | GPIOEAF_PRINT_EVENT  */
	}
};

static struct gpio_event_direct_entry sapphire_nav_buttons[] = {
	{ SAPPHIRE_GPIO_NAVI_ACT_N, BTN_MOUSE },
};

static struct gpio_event_input_info sapphire_nav_button_info = {
	.info.func = gpio_event_input_func,
	.flags = GPIOEDF_PRINT_KEYS | GPIOEDF_PRINT_KEY_DEBOUNCE,
	.poll_time.tv.nsec = 40 * NSEC_PER_MSEC,
	.type = EV_KEY,
	.keymap = sapphire_nav_buttons,
	.keymap_size = ARRAY_SIZE(sapphire_nav_buttons)
};

static struct gpio_event_info *sapphire_nav_info[] = {
	&sapphire_x_axis.info.info,
	&sapphire_y_axis.info.info,
	&sapphire_nav_button_info.info
};

static struct gpio_event_platform_data sapphire_nav_data = {
	.name = "sapphire-nav",
	.info = sapphire_nav_info,
	.info_count = ARRAY_SIZE(sapphire_nav_info),
	.power = sapphire_nav_power,
};

static struct platform_device sapphire_nav_device = {
	.name = GPIO_EVENT_DEV_NAME,
	.id = 2,
	.dev = {
		.platform_data = &sapphire_nav_data,
	},
};

/* a new search button to be a wake-up source */
static struct gpio_event_direct_entry sapphire_search_button_v1[] = {
	{ SAPPHIRE_GPIO_SEARCH_ACT_N, KEY_COMPOSE }, /* CPLD Key Search*/
};

static struct gpio_event_direct_entry sapphire_search_button_v2[] = {
	{ SAPPHIRE_GPIO_SEARCH_ACT_N, KEY_HOME }, /* CPLD Key Home */
};

static struct gpio_event_input_info sapphire_search_button_info = {
	.info.func = gpio_event_input_func,
	/* .flags = GPIOEDF_PRINT_KEYS | GPIOEDF_PRINT_KEY_DEBOUNCE, */
	.flags = 0,
	.poll_time.tv.nsec = 40 * NSEC_PER_MSEC,
	.type = EV_KEY,
	.keymap = sapphire_search_button_v2,
	.keymap_size = ARRAY_SIZE(sapphire_search_button_v2)
};

static struct gpio_event_info *sapphire_search_info[] = {
	&sapphire_search_button_info.info
};

static struct gpio_event_platform_data sapphire_search_button_data = {
	.name = "sapphire-nav-button",
	.info = sapphire_search_info,
	.info_count = ARRAY_SIZE(sapphire_search_info),
};

static struct platform_device sapphire_search_button_device = {
	.name = GPIO_EVENT_DEV_NAME,
	.id = 1,
	.dev = {
		.platform_data = &sapphire_search_button_data,
	},
};

static int sapphire_reset_keys_up[] = {
	BTN_MOUSE,
	0
};

static struct keyreset_platform_data sapphire_reset_keys_pdata = {
	.keys_up = sapphire_reset_keys_up,
	.keys_down = {
		KEY_SEND,
		KEY_MENU,
		KEY_END,
		0
	},
};

struct platform_device sapphire_reset_keys_device = {
	.name = KEYRESET_NAME,
	.dev.platform_data = &sapphire_reset_keys_pdata,
};
static ssize_t htc_battery_show_attr(struct device *dev,
					 struct device_attribute *attr,
					 char *buf);
static struct htc_battery_platform_data htc_battery_pdev_data = {
	.func_show_batt_attr = htc_battery_show_attr,
	.guage_driver = GUAGE_MODEM,
	.charger = LINEAR_CHARGER,
	.m2a_cable_detect = 1,
};

static struct platform_device htc_battery_pdev = {
	.name = "htc_battery",
	.id = -1,
	.dev	= {
		.platform_data = &htc_battery_pdev_data,
	},
};


static int sapphire_ts_power(int on)
{
	int gpio_tp_ls_en = SAPPHIRE_TP_LS_EN;
	if (is_sapphire_gpiov20())
		gpio_tp_ls_en = SAPPHIRE20_TP_LS_EN;

	if (on) {
		sapphire_gpio_write(NULL, SAPPHIRE_GPIO_TP_EN, 1);
		/* touchscreen must be powered before we enable i2c pullup */
		msleep(2);
		/* enable touch panel level shift */
		gpio_set_value(gpio_tp_ls_en, 1);
		msleep(2);
	}
	else {
		gpio_set_value(gpio_tp_ls_en, 0);
		sapphire_gpio_write(NULL, SAPPHIRE_GPIO_TP_EN, 0);
	}

	return 0;
}

static struct synaptics_i2c_rmi_platform_data sapphire_ts_data[] = {
{
		.version = 0x0101,
		.power = sapphire_ts_power,
		.sensitivity = 7,
		.flags = SYNAPTICS_FLIP_Y | SYNAPTICS_SNAP_TO_INACTIVE_EDGE,
		.inactive_left = -50 * 0x10000 / 4334,
		.inactive_right = -50 * 0x10000 / 4334,
		.inactive_top = -40 * 0x10000 / 6696,
		.inactive_bottom = -40 * 0x10000 / 6696,
		.snap_left_on = 50 * 0x10000 / 4334,
		.snap_left_off = 60 * 0x10000 / 4334,
		.snap_right_on = 50 * 0x10000 / 4334,
		.snap_right_off = 60 * 0x10000 / 4334,
		.snap_top_on = 100 * 0x10000 / 6696,
		.snap_top_off = 110 * 0x10000 / 6696,
		.snap_bottom_on = 100 * 0x10000 / 6696,
		.snap_bottom_off = 110 * 0x10000 / 6696,
		.display_width = 320,
		.display_height = 480,
	},
	{
		.flags = SYNAPTICS_FLIP_Y | SYNAPTICS_SNAP_TO_INACTIVE_EDGE,
		.inactive_left = ((4674 - 4334) / 2 + 200) * 0x10000 / 4334,
		.inactive_right = ((4674 - 4334) / 2 + 200) * 0x10000 / 4334,
		.inactive_top = ((6946 - 6696) / 2) * 0x10000 / 6696,
		.inactive_bottom = ((6946 - 6696) / 2) * 0x10000 / 6696,
		.display_width = 320,
		.display_height = 480,
	}
};

static struct akm8976_platform_data compass_platform_data = {
	.reset = SAPPHIRE_GPIO_COMPASS_RST_N,
	.clk_on = SAPPHIRE_GPIO_COMPASS_32K_EN,
	.intr = SAPPHIRE_GPIO_COMPASS_IRQ,
};

static struct elan_i2c_platform_data elan_i2c_data[] = {
	{
		.version = 0x104,
		.abs_x_min = 0,
		.abs_y_min = 0,
		.intr_gpio = SAPPHIRE_GPIO_TP_ATT_N,
		.power = sapphire_ts_power,
		.display_width = 320,
		.display_height = 480,
	},
	{
		.version = 0x103,
		.abs_x_min = 0,
		.abs_x_max = 512 * 2,
		.abs_y_min = 0,
		.abs_y_max = 896 * 2,
		.intr_gpio = SAPPHIRE_GPIO_TP_ATT_N,
		.power = sapphire_ts_power,
		.display_width = 320,
		.display_height = 480,
	},
	{
		.version = 0x102,
		.abs_x_min = 0,
		.abs_x_max = 384,
		.abs_y_min = 0,
		.abs_y_max = 576,
		.intr_gpio = SAPPHIRE_GPIO_TP_ATT_N,
		.power = sapphire_ts_power,
		.display_width = 320,
		.display_height = 480,
	},
	{
		.version = 0x101,
		.abs_x_min = 32 + 1,
		.abs_x_max = 352 - 1,
		.abs_y_min = 32 + 1,
		.abs_y_max = 544 - 1,
		.intr_gpio = SAPPHIRE_GPIO_TP_ATT_N,
		.power = sapphire_ts_power,
		.display_width = 320,
		.display_height = 480,
	}
};

static struct i2c_board_info i2c_devices[] = {
	{
		I2C_BOARD_INFO(SYNAPTICS_I2C_RMI_NAME, 0x20),
		.platform_data = sapphire_ts_data,
		.irq = SAPPHIRE_GPIO_TO_INT(SAPPHIRE_GPIO_TP_ATT_N)
	},
	{
		I2C_BOARD_INFO(ELAN_8232_I2C_NAME, 0x10),
		.platform_data = &elan_i2c_data,
		.irq = SAPPHIRE_GPIO_TO_INT(SAPPHIRE_GPIO_TP_ATT_N),
	},
	{
		I2C_BOARD_INFO("akm8976", 0x1C),
		.platform_data = &compass_platform_data,
		.irq = SAPPHIRE_GPIO_TO_INT(SAPPHIRE_GPIO_COMPASS_IRQ),
	},
};

static struct i2c_board_info i2c_mt9p012 = {
	I2C_BOARD_INFO("mt9p012", 0x6C >> 1),
};

static struct i2c_board_info i2c_mt9t013 = {
	I2C_BOARD_INFO("mt9t013", 0x6C), /* sync from paradise */
};

#ifdef CONFIG_LEDS_CPLD
static struct resource cpldled_resources[] = {
	{
		.start	= SAPPHIRE_CPLD_LED_BASE,
		.end	= SAPPHIRE_CPLD_LED_BASE + SAPPHIRE_CPLD_LED_SIZE - 1,
		.flags	= IORESOURCE_MEM,
	}
};

static struct platform_device android_CPLD_leds = {
	.name		= "leds-cpld",
	.id			= -1,
	.num_resources	= ARRAY_SIZE(cpldled_resources),
	.resource	= cpldled_resources,
};
#endif

static struct gpio_led android_led_list[] = {
	{
		.name = "button-backlight",
		.gpio = SAPPHIRE_GPIO_APKEY_LED_EN,
	},
};

static struct gpio_led_platform_data android_leds_data = {
	.num_leds	= ARRAY_SIZE(android_led_list),
	.leds		= android_led_list,
};

static struct platform_device android_leds = {
	.name		= "leds-gpio",
	.id		= -1,
	.dev		= {
		.platform_data = &android_leds_data,
	},
};

#ifdef CONFIG_HTC_HEADSET_V1
/* RTS/CTS to GPO/GPI. */
static uint32_t uart1_on_gpio_table[] = {
	/* allenou, uart hs test, 2008/11/18 */
	#ifdef CONFIG_SERIAL_MSM_HS
	/* RTS */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_UART1_RTS, 2,
		      GPIO_OUTPUT, GPIO_PULL_UP, GPIO_8MA),
	/* CTS */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_UART1_CTS, 2,
		      GPIO_INPUT, GPIO_PULL_UP, GPIO_8MA),
	#else
	/* RTS */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_UART1_RTS, 1,
		      GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA),
	/* CTS */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_UART1_CTS, 1,
		      GPIO_INPUT, GPIO_PULL_DOWN, GPIO_4MA),
	#endif
};

/* RTS,CTS to BT. */
static uint32_t uart1_off_gpio_table[] = {
	/* RTS */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_UART1_RTS, 0,
		      GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA),
	/* CTS */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_UART1_CTS, 0,
		      GPIO_INPUT, GPIO_NO_PULL, GPIO_2MA),
};

/* Sapphire: Switch between UART3 and GPIO */
static uint32_t uart3_on_gpio_table[] = {
	/* RX */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_UART3_RX, 1,
		      GPIO_INPUT, GPIO_NO_PULL, GPIO_2MA),
	/* TX */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_UART3_TX, 1,
		      GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA),
};

/* set TX,RX to GPI */
static uint32_t uart3_off_gpi_table[] = {
	/* RX, H2W DATA */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_H2W_DATA, 0,
		      GPIO_INPUT, GPIO_NO_PULL, GPIO_2MA),
	/* TX, H2W CLK */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_H2W_CLK, 0,
		      GPIO_INPUT, GPIO_KEEPER, GPIO_2MA),
};

/* set TX,RX to GPO */
static uint32_t uart3_off_gpo_table[] = {
	/* RX, H2W DATA */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_H2W_DATA, 0,
		      GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA),
	/* TX, H2W CLK */
	PCOM_GPIO_CFG(SAPPHIRE_GPIO_H2W_CLK, 0,
		      GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA),
};

static int sapphire_h2w_path = H2W_GPIO;

static void configure_cpld(int route)
{
	switch (route) {
	case H2W_UART1:
		/* Make sure uart1 funtion pin opened. */
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+1, 0);
		gpio_set_value(SAPPHIRE_GPIO_H2W_SEL0, 1);
		gpio_set_value(SAPPHIRE_GPIO_H2W_SEL1, 0);
		sapphire_h2w_path = H2W_UART1;
		printk(KERN_INFO "H2W route = H2W-UART1, BT-X, UART3-X \n");
		break;
	case H2W_BT:
		gpio_set_value(SAPPHIRE_GPIO_H2W_SEL0, 1);
		gpio_set_value(SAPPHIRE_GPIO_H2W_SEL1, 1);
		/* UART1 RTS/CTS to GPO/GPI. */
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_off_gpio_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_off_gpio_table+1, 0);
		sapphire_h2w_path = H2W_BT;
		printk(KERN_INFO "H2W route = H2W-BT, UART1-X, UART3-X \n");
		break;
	case H2W_GPIO: /*H2W_UART3 TX,RX are changed to H2W_GPIO */
		gpio_set_value(SAPPHIRE_GPIO_H2W_SEL0, 0);
		gpio_set_value(SAPPHIRE_GPIO_H2W_SEL1, 0);
		/* Set the CPLD connected H2W GPIO's to input */
		gpio_set_value(SAPPHIRE_GPIO_H2W_CLK_DIR, 0);
		gpio_set_value(SAPPHIRE_GPIO_H2W_DAT_DIR, 0);
		/* TX,RX GPI first. */
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart3_off_gpi_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart3_off_gpi_table+1, 0);
		/* Make sure uart1 funtion pin opened. */
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+1, 0);
		sapphire_h2w_path = H2W_GPIO;
		printk(KERN_INFO "H2W route = H2W-GPIO, BT-UART1 \n");
		break;
	case H2W_UART3:
	default:
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart3_on_gpio_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart3_on_gpio_table+1, 0);
		gpio_set_value(SAPPHIRE_GPIO_H2W_SEL0, 0);
		gpio_set_value(SAPPHIRE_GPIO_H2W_SEL1, 1);
		/* Make sure uart1 funtion pin opened. */
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+0, 0);
		msm_proc_comm(PCOM_RPC_GPIO_TLMM_CONFIG_EX,
			      uart1_on_gpio_table+1, 0);
		sapphire_h2w_path = H2W_UART3;
		printk(KERN_INFO "H2W route = H2W-UART3, BT-UART1 \n");
		break;
	}
}

static void h2w_defconfig(void)
{
	configure_cpld(H2W_GPIO);
}

static void set_h2w_dat(int n)
{
	gpio_set_value(SAPPHIRE_GPIO_H2W_DATA, n);
}

static void set_h2w_clk(int n)
{
	gpio_set_value(SAPPHIRE_GPIO_H2W_CLK, n);
}

static void set_h2w_dat_dir(int n)
{
	if (n == 0) /* input */
		gpio_direction_input(SAPPHIRE_GPIO_H2W_DATA);
	else
		gpio_configure(SAPPHIRE_GPIO_H2W_DATA, GPIOF_DRIVE_OUTPUT);

	gpio_set_value(SAPPHIRE_GPIO_H2W_DAT_DIR, n);

}

static void set_h2w_clk_dir(int n)
{
	if (n == 0) /* input */
		gpio_direction_input(SAPPHIRE_GPIO_H2W_CLK);
	else
		gpio_configure(SAPPHIRE_GPIO_H2W_CLK, GPIOF_DRIVE_OUTPUT);

	gpio_set_value(SAPPHIRE_GPIO_H2W_CLK_DIR, n);
}

static int get_h2w_dat(void)
{
	return gpio_get_value(SAPPHIRE_GPIO_H2W_DATA);
}

static int get_h2w_clk(void)
{
	return gpio_get_value(SAPPHIRE_GPIO_H2W_CLK);
}

static int set_h2w_path(const char *val, struct kernel_param *kp)
{
	int ret = -EINVAL;

	ret = param_set_int(val, kp);
	if (ret)
		return ret;

	switch (sapphire_h2w_path) {
	case H2W_GPIO:
	case H2W_UART1:
	case H2W_UART3:
	case H2W_BT:
		break;
	default:
		sapphire_h2w_path = -1;
		return -EINVAL;
	}

	configure_cpld(sapphire_h2w_path);
	return ret;
}
module_param_call(h2w_path, set_h2w_path, param_get_int,
		&sapphire_h2w_path, S_IWUSR | S_IRUGO);

static struct h2w_platform_data sapphire_h2w_data = {
	.h2w_power		= SAPPHIRE_GPIO_H2W_POWER,
	.cable_in1		= SAPPHIRE_GPIO_CABLE_IN1,
	.cable_in2		= SAPPHIRE_GPIO_CABLE_IN2,
	.h2w_clk		= SAPPHIRE_GPIO_H2W_CLK,
	.h2w_data		= SAPPHIRE_GPIO_H2W_DATA,
	.headset_mic_35mm	= SAPPHIRE_GPIO_AUD_HSMIC_DET_N,
	.debug_uart 		= H2W_UART3,
	.config 		= configure_cpld,
	.defconfig 		= h2w_defconfig,
	.set_dat		= set_h2w_dat,
	.set_clk		= set_h2w_clk,
	.set_dat_dir		= set_h2w_dat_dir,
	.set_clk_dir		= set_h2w_clk_dir,
	.get_dat		= get_h2w_dat,
	.get_clk		= get_h2w_clk,
	.flags	= HTC_11PIN_HEADSET_SUPPORT | HTC_H2W_SUPPORT,
};

static struct platform_device sapphire_h2w = {
	.name		= "h2w",
	.id			= -1,
	.dev		= {
		.platform_data	= &sapphire_h2w_data,
	},
};
#endif	/* CONFIG_HTC_HEADSET_V1 */

static void sapphire_phy_reset(void)
{
	gpio_set_value(SAPPHIRE_GPIO_USB_PHY_RST_N, 0);
	mdelay(10);
	gpio_set_value(SAPPHIRE_GPIO_USB_PHY_RST_N, 1);
	mdelay(10);
}


static struct pwr_sink sapphire_pwrsink_table[] = {
	{
		.id	= PWRSINK_AUDIO,
		.ua_max	= 100000,
	},
	{
		.id	= PWRSINK_BACKLIGHT,
		.ua_max	= 125000,
	},
	{
		.id	= PWRSINK_LED_BUTTON,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_LED_KEYBOARD,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_GP_CLK,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_BLUETOOTH,
		.ua_max	= 15000,
	},
	{
		.id	= PWRSINK_CAMERA,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_SDCARD,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_VIDEO,
		.ua_max	= 0,
	},
	{
		.id	= PWRSINK_WIFI,
		.ua_max = 200000,
	},
	{
		.id	= PWRSINK_SYSTEM_LOAD,
		.ua_max	= 100000,
		.percent_util = 38,
	},
};


static struct pwr_sink_platform_data sapphire_pwrsink_data = {
	.num_sinks	= ARRAY_SIZE(sapphire_pwrsink_table),
	.sinks		= sapphire_pwrsink_table,
};

static struct platform_device sapphire_pwr_sink = {
	.name = "htc_pwrsink",
	.id = -1,
	.dev	= {
		.platform_data = &sapphire_pwrsink_data,
	},
};

static int sapphire_phy_init_seq[] = {0x40, 0x31, 0x1, 0x0D, 0x1, 0x10, -1};

static struct platform_device sapphire_rfkill = {
	.name = "sapphire_rfkill",
	.id = -1,
};

static struct msm_pmem_setting pmem_setting_32 = {
	.pmem_start = SMI32_MSM_PMEM_MDP_BASE,
	.pmem_size = SMI32_MSM_PMEM_MDP_SIZE,
	.pmem_adsp_start = SMI32_MSM_PMEM_ADSP_BASE,
	.pmem_adsp_size = SMI32_MSM_PMEM_ADSP_SIZE,
	.pmem_gpu0_start = MSM_PMEM_GPU0_BASE,
	.pmem_gpu0_size = MSM_PMEM_GPU0_SIZE,
	.pmem_gpu1_start = SMI32_MSM_PMEM_GPU1_BASE,
	.pmem_gpu1_size = SMI32_MSM_PMEM_GPU1_SIZE,
	.pmem_camera_start = SMI32_MSM_PMEM_CAMERA_BASE,
	.pmem_camera_size = SMI32_MSM_PMEM_CAMERA_SIZE,
	.ram_console_start = SMI32_MSM_RAM_CONSOLE_BASE,
	.ram_console_size = SMI32_MSM_RAM_CONSOLE_SIZE,
};

static struct msm_pmem_setting pmem_setting_64 = {
	.pmem_start = SMI64_MSM_PMEM_MDP_BASE,
	.pmem_size = SMI64_MSM_PMEM_MDP_SIZE,
	.pmem_adsp_start = SMI64_MSM_PMEM_ADSP_BASE,
	.pmem_adsp_size = SMI64_MSM_PMEM_ADSP_SIZE,
	.pmem_gpu0_start = MSM_PMEM_GPU0_BASE,
	.pmem_gpu0_size = MSM_PMEM_GPU0_SIZE,
	.pmem_gpu1_start = SMI64_MSM_PMEM_GPU1_BASE,
	.pmem_gpu1_size = SMI64_MSM_PMEM_GPU1_SIZE,
	.pmem_camera_start = SMI64_MSM_PMEM_CAMERA_BASE,
	.pmem_camera_size = SMI64_MSM_PMEM_CAMERA_SIZE,
	.ram_console_start = SMI64_MSM_RAM_CONSOLE_BASE,
	.ram_console_size = SMI64_MSM_RAM_CONSOLE_SIZE,
};

static struct msm_i2c_device_platform_data sapphire_i2c_device_data = {
	.i2c_clock = 100000,
	.clock_strength = GPIO_8MA,
	.data_strength = GPIO_4MA,
};

#ifdef CONFIG_MSM_CAMERA
void config_sapphire_camera_on_gpios(void);
void config_sapphire_camera_on_gpios(void);
static struct msm_camera_device_platform_data msm_camera_device_data = {
	.camera_gpio_on  = config_sapphire_camera_on_gpios,
	.camera_gpio_off = config_sapphire_camera_off_gpios,
	.ioext.mdcphy = MSM_MDC_PHYS,
	.ioext.mdcsz  = MSM_MDC_SIZE,
	.ioext.appphy = MSM_CLK_CTL_PHYS,
	.ioext.appsz  = MSM_CLK_CTL_SIZE,
};

#ifdef CONFIG_MT9T013
static struct msm_camera_sensor_info msm_camera_sensor_mt9t013_data = {
	.sensor_name    = "mt9t013",
	.sensor_reset   = 108,
	.sensor_pwd     = 85,
	.vcm_pwd        = SAPPHIRE_GPIO_VCM_PWDN,
	.pdata          = &msm_camera_device_data,
};

static struct platform_device msm_camera_sensor_mt9t013 = {
	.name           = "msm_camera_mt9t013",
	.dev            = {
		.platform_data = &msm_camera_sensor_mt9t013_data,
	},
};
#endif

#ifdef CONFIG_MT9P012
static struct msm_camera_sensor_info msm_camera_sensor_mt9p012_data = {
	.sensor_name	= "mt9p012",
	.sensor_reset	= 108,
	.sensor_pwd	= 85,
	.vcm_pwd        = SAPPHIRE_GPIO_VCM_PWDN,
	.pdata		= &msm_camera_device_data,
};

static struct platform_device msm_camera_sensor_mt9p012 = {
	.name           = "msm_camera_mt9p012",
	.dev            = {
		.platform_data = &msm_camera_sensor_mt9p012_data,
	},
};
#endif

#endif/*CONFIG_MSM_CAMERA*/

static struct platform_device *devices[] __initdata = {
	&msm_device_i2c,
	&htc_battery_pdev,
	&sapphire_nav_device,
	&sapphire_search_button_device,
	&sapphire_reset_keys_device,
	&android_leds,
	&sapphire_h2w,
#ifdef CONFIG_LEDS_CPLD
	&android_CPLD_leds,
#endif
#ifdef CONFIG_MT9T013
	&msm_camera_sensor_mt9t013,
#endif
#ifdef CONFIG_MT9P012
	&msm_camera_sensor_mt9p012,
#endif
	&sapphire_rfkill,

#ifdef CONFIG_HTC_PWRSINK
	&sapphire_pwr_sink,
#endif
};

extern struct sys_timer msm_timer;

static void __init sapphire_init_irq(void)
{
	printk(KERN_DEBUG "sapphire_init_irq()\n");
	msm_init_irq();
}

static uint cpld_iset;
static uint cpld_charger_en;
static uint cpld_usb_h2w_sw;
static uint opt_disable_uart3;

module_param_named(iset, cpld_iset, uint, 0);
module_param_named(charger_en, cpld_charger_en, uint, 0);
module_param_named(usb_h2w_sw, cpld_usb_h2w_sw, uint, 0);
module_param_named(disable_uart3, opt_disable_uart3, uint, 0);
static char bt_chip_id[10] = "brfxxxx";
module_param_string(bt_chip_id, bt_chip_id, sizeof(bt_chip_id), S_IWUSR | S_IRUGO);
MODULE_PARM_DESC(bt_chip_id, "BT's chip id");

static char bt_fw_version[10] = "v2.0.38";
module_param_string(bt_fw_version, bt_fw_version, sizeof(bt_fw_version), S_IWUSR | S_IRUGO);
MODULE_PARM_DESC(bt_fw_version, "BT's fw version");

static void sapphire_reset(void)
{
	gpio_set_value(SAPPHIRE_GPIO_PS_HOLD, 0);
}

static uint32_t gpio_table[] = {
	/* BLUETOOTH */
#ifdef CONFIG_SERIAL_MSM_HS
	PCOM_GPIO_CFG(43, 2, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* RTS */
	PCOM_GPIO_CFG(44, 2, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* CTS */
	PCOM_GPIO_CFG(45, 2, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* RX */
	PCOM_GPIO_CFG(46, 3, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* TX */
#else
	PCOM_GPIO_CFG(43, 1, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* RTS */
	PCOM_GPIO_CFG(44, 1, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* CTS */
	PCOM_GPIO_CFG(45, 1, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* RX */
	PCOM_GPIO_CFG(46, 1, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_4MA), /* TX */
#endif
};


static uint32_t camera_off_gpio_table[] = {
	/* CAMERA */
	PCOM_GPIO_CFG(2, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT2 */
	PCOM_GPIO_CFG(3, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT3 */
	PCOM_GPIO_CFG(4, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT4 */
	PCOM_GPIO_CFG(5, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT5 */
	PCOM_GPIO_CFG(6, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT6 */
	PCOM_GPIO_CFG(7, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT7 */
	PCOM_GPIO_CFG(8, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT8 */
	PCOM_GPIO_CFG(9, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT9 */
	PCOM_GPIO_CFG(10, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT10 */
	PCOM_GPIO_CFG(11, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT11 */
	PCOM_GPIO_CFG(12, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* PCLK */
	PCOM_GPIO_CFG(13, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* HSYNC_IN */
	PCOM_GPIO_CFG(14, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* VSYNC_IN */
	PCOM_GPIO_CFG(15, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* MCLK */
};

static uint32_t camera_on_gpio_table[] = {
	/* CAMERA */
	PCOM_GPIO_CFG(2, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT2 */
	PCOM_GPIO_CFG(3, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT3 */
	PCOM_GPIO_CFG(4, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT4 */
	PCOM_GPIO_CFG(5, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT5 */
	PCOM_GPIO_CFG(6, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT6 */
	PCOM_GPIO_CFG(7, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT7 */
	PCOM_GPIO_CFG(8, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT8 */
	PCOM_GPIO_CFG(9, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT9 */
	PCOM_GPIO_CFG(10, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT10 */
	PCOM_GPIO_CFG(11, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT11 */
	PCOM_GPIO_CFG(12, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_16MA), /* PCLK */
	PCOM_GPIO_CFG(13, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* HSYNC_IN */
	PCOM_GPIO_CFG(14, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* VSYNC_IN */
	PCOM_GPIO_CFG(15, 1, GPIO_OUTPUT, GPIO_PULL_DOWN, GPIO_16MA), /* MCLK */
};

static uint32_t camera_off_gpio_12pins_table[] = {
	/* CAMERA */
	PCOM_GPIO_CFG(0, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT0 */
	PCOM_GPIO_CFG(1, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT1 */
	PCOM_GPIO_CFG(2, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT2 */
	PCOM_GPIO_CFG(3, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT3 */
	PCOM_GPIO_CFG(4, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT4 */
	PCOM_GPIO_CFG(5, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT5 */
	PCOM_GPIO_CFG(6, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT6 */
	PCOM_GPIO_CFG(7, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT7 */
	PCOM_GPIO_CFG(8, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT8 */
	PCOM_GPIO_CFG(9, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT9 */
	PCOM_GPIO_CFG(10, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT10 */
	PCOM_GPIO_CFG(11, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* DAT11 */
	PCOM_GPIO_CFG(12, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* PCLK */
	PCOM_GPIO_CFG(13, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* HSYNC_IN */
	PCOM_GPIO_CFG(14, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* VSYNC_IN */
	PCOM_GPIO_CFG(15, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_4MA), /* MCLK */
};

static uint32_t camera_on_gpio_12pins_table[] = {
	/* CAMERA */
	PCOM_GPIO_CFG(0, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT0 */
	PCOM_GPIO_CFG(1, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT1 */
	PCOM_GPIO_CFG(2, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT2 */
	PCOM_GPIO_CFG(3, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT3 */
	PCOM_GPIO_CFG(4, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT4 */
	PCOM_GPIO_CFG(5, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT5 */
	PCOM_GPIO_CFG(6, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT6 */
	PCOM_GPIO_CFG(7, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT7 */
	PCOM_GPIO_CFG(8, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT8 */
	PCOM_GPIO_CFG(9, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT9 */
	PCOM_GPIO_CFG(10, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT10 */
	PCOM_GPIO_CFG(11, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* DAT11 */
	PCOM_GPIO_CFG(12, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_16MA), /* PCLK */
	PCOM_GPIO_CFG(13, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* HSYNC_IN */
	PCOM_GPIO_CFG(14, 1, GPIO_INPUT, GPIO_PULL_DOWN, GPIO_2MA), /* VSYNC_IN */
	PCOM_GPIO_CFG(15, 1, GPIO_OUTPUT, GPIO_PULL_DOWN, GPIO_16MA), /* MCLK */
};

void config_sapphire_camera_on_gpios(void)
{
	/*Add for judage it's 10 pins or 12 pins platform ----->*/
	if  (is_12pin_camera()) {
		config_gpio_table(camera_on_gpio_12pins_table,
				ARRAY_SIZE(camera_on_gpio_12pins_table));
	} else {
		config_gpio_table(camera_on_gpio_table,
				ARRAY_SIZE(camera_on_gpio_table));
	}
	/*End Of Add for judage it's 10 pins or 12 pins platform*/
}

void config_sapphire_camera_off_gpios(void)
{
	/*Add for judage it's 10 pins or 12 pins platform ----->*/
	if (is_12pin_camera()) {
		config_gpio_table(camera_off_gpio_12pins_table,
		ARRAY_SIZE(camera_off_gpio_12pins_table));
	} else {
		config_gpio_table(camera_off_gpio_table,
		ARRAY_SIZE(camera_off_gpio_table));
	}
	/*End Of Add for judage it's 10 pins or 12 pins platform*/
}

static void __init config_gpios(void)
{
	config_gpio_table(gpio_table, ARRAY_SIZE(gpio_table));
	config_sapphire_camera_off_gpios();
}

static struct msm_acpu_clock_platform_data sapphire_clock_data = {
	.acpu_switch_time_us = 20,
	.max_speed_delta_khz = 256000,
	.vdd_switch_time_us = 62,
	.power_collapse_khz = 19200,
#if defined(CONFIG_TURBO_MODE)
	.wait_for_irq_khz = 176000,
#else
	.wait_for_irq_khz = 128000,
#endif
};

static unsigned sapphire_perf_acpu_table[] = {
	245760000,
	480000000,
	528000000,
};

static struct perflock_platform_data sapphire_perflock_data = {
	.perf_acpu_table = sapphire_perf_acpu_table,
	.table_size = ARRAY_SIZE(sapphire_perf_acpu_table),
};

#ifdef CONFIG_SERIAL_MSM_HS
static struct msm_serial_hs_platform_data msm_uart_dm1_pdata = {
	.wakeup_irq = MSM_GPIO_TO_INT(45),
	.inject_rx_on_wakeup = 1,
	.rx_to_inject = 0x32,
	.cpu_lock_supported = 1,
};
#endif

static void __init sapphire_init(void)
{
	int rc;
	printk("sapphire_init() revision = 0x%X\n", system_rev);
	config_gpios();

	msm_hw_reset_hook = sapphire_reset;

	msm_acpu_clock_init(&sapphire_clock_data);
	perflock_init(&sapphire_perflock_data);

	/* adjust GPIOs based on bootloader request */
	printk("sapphire_init: cpld_usb_hw2_sw = %d\n", cpld_usb_h2w_sw);
	gpio_set_value(SAPPHIRE_GPIO_USB_H2W_SW, cpld_usb_h2w_sw);

#if defined(CONFIG_MSM_SERIAL_DEBUGGER)
	if (!opt_disable_uart3)
		msm_serial_debug_init(MSM_UART3_PHYS, INT_UART3,
				      &msm_device_uart3.dev, 1, INT_UART3_RX);
#endif

	/* H2W pins <-> UART3, Bluetooth <-> UART1 */
	gpio_set_value(SAPPHIRE_GPIO_H2W_SEL0, 0);
	gpio_set_value(SAPPHIRE_GPIO_H2W_SEL1, 1);
	/* put the AF VCM in powerdown mode to avoid noise */
	if (sapphire_is_5M_camera())
		sapphire_gpio_write(NULL, SAPPHIRE_GPIO_VCM_PWDN, 0);
	else
		sapphire_gpio_write(NULL, SAPPHIRE_GPIO_VCM_PWDN, 1);
	mdelay(100);
	msm_add_devices();

#ifdef CONFIG_SERIAL_MSM_HS
	msm_device_uart_dm1.dev.platform_data = &msm_uart_dm1_pdata;
	msm_add_serial_devices(MSM_SERIAL_UART1DM);
	#else
	msm_add_serial_devices(MSM_SERIAL_UART1);
#endif

	msm_add_serial_devices(MSM_SERIAL_UART3);

	msm_register_usb_phy_init_seq(sapphire_phy_init_seq);
	msm_add_usb_devices(sapphire_phy_reset, NULL);

	if (32 == smi_sz)
		msm_add_mem_devices(&pmem_setting_32);
	else
		msm_add_mem_devices(&pmem_setting_64);

	msm_init_pmic_vibrator();

	rc = sapphire_init_mmc(system_rev);
	if (rc)
		printk(KERN_CRIT "%s: MMC init failure (%d)\n", __func__, rc);

	msm_device_i2c.dev.platform_data = &sapphire_i2c_device_data;

	if( (sapphire_get_skuid() != 0x22800) &&
		(system_rev != 0x80) &&
		((sapphire_get_skuid() & 0xFFFFFF00) != 0x00021400) &&
		(sapphire_get_skuid() != 0x1E303) &&
		(sapphire_get_skuid() != 0x1FF01))
		sapphire_search_button_info.keymap = sapphire_search_button_v1;

	platform_add_devices(devices, ARRAY_SIZE(devices));
	i2c_register_board_info(0, i2c_devices, ARRAY_SIZE(i2c_devices));
	if  (sapphire_is_5M_camera())
		i2c_register_board_info(0, &i2c_mt9p012, 1);
	else
		i2c_register_board_info(0, &i2c_mt9t013, 1);

	if (is_sapphire_gpiov20())
		gpio_direction_output(SAPPHIRE20_TP_LS_EN, 0);
	else
		gpio_direction_output(SAPPHIRE_TP_LS_EN, 0);
}

static struct map_desc sapphire_io_desc[] __initdata = {
	{
		.virtual = SAPPHIRE_CPLD_BASE,
		.pfn     = __phys_to_pfn(SAPPHIRE_CPLD_START),
		.length  = SAPPHIRE_CPLD_SIZE,
		.type    = MT_DEVICE_NONSHARED
	}
};


unsigned int sapphire_get_hwid(void)
{
	printk("sapphire_get_hwid=0x%x\r\n", hwid);
	return hwid;
}

unsigned int sapphire_get_skuid(void)
{
	printk("sapphire_get_skuid=0x%x\r\n", skuid);
	return skuid;
}

unsigned sapphire_engineerid(void)
{
	printk("sapphire_engineerid=0x%x\r\n", engineerid);
	return engineerid;
}

int sapphire_is_5M_camera(void)
{
	int ret = 0;
	if (sapphire_get_skuid() == 0x1FF00 && !(sapphire_engineerid() & 0x02))
		ret = 1;
	else if (sapphire_get_skuid() == 0x20100 && !(sapphire_engineerid() & 0x02))
		ret = 1;
	printk("sapphire_is_5M_camera=%d\n", ret);
	return ret;
}

/* it can support 3M and 5M sensor */
unsigned int is_12pin_camera(void)
{
	unsigned int ret = 0;

    if (sapphire_get_skuid() == 0x1FF00 ||
    	sapphire_get_skuid() == 0x20100)
		ret = 1;
	else
		ret = 0;
    printk("is_12pin_camera=%d\r\n", ret);
	return ret;
}

unsigned int is_sapphire_gpiov20(void)
{
    unsigned int ret = 0;

    if (sapphire_get_skuid() == 0x1FF00 ||
    	sapphire_get_skuid() == 0x20100 ||
    	sapphire_get_skuid() == 0x22800
    	)
        ret = 1;
    else
        ret = 0;
    printk("is_sapphire_gpiov20=%d\r\n", ret);
    return ret;
}

int sapphire_get_smi_size(void)
{
	printk(KERN_DEBUG "get_smi_size=%d\n", smi_sz);
	return smi_sz;
}

static void __init sapphire_fixup(struct machine_desc *desc, struct tag *tags,
				  char **cmdline, struct meminfo *mi)
{
	smi_sz = parse_tag_smi((const struct tag *)tags);
	printk("sapphire_fixup:smisize=%d\n", smi_sz);
	hwid = parse_tag_hwid((const struct tag *)tags);
	printk("sapphire_fixup:hwid=0x%x\n", hwid);
	skuid = parse_tag_skuid((const struct tag *)tags);
	printk("sapphire_fixup:skuid=0x%x\n", skuid);
	engineerid = parse_tag_engineerid((const struct tag *)tags);
	printk("sapphire_fixup:engineerid=0x%x\n", engineerid);

	mi->nr_banks = 1;
	mi->bank[0].start = PHYS_OFFSET;
	mi->bank[0].node = PHYS_TO_NID(PHYS_OFFSET);
#if defined(CONFIG_MSM_AMSS_SUPPORT_256MB_EBI1)
	if (32 == smi_sz) {
#ifdef CONFIG_32A_RAMHACK
                mi->nr_banks = 2;
#endif /* 32A RamHack */
		mi->bank[0].size = MSM_EBI_SMI32_256MB_SIZE;
#ifdef CONFIG_32A_RAMHACK
                mi->bank[1].start = 0x27000000;
                mi->bank[1].size  = 0xa00000;
#endif /* 32A RamHack */
	} else if (64 == smi_sz){
		mi->bank[0].size = MSM_EBI_SMI64_128MB_SIZE;
	} else {
		printk(KERN_ERR "can not get smi size\n");

		/*Give a default value when not get smi size*/
		smi_sz = 64;
		mi->bank[0].size = MSM_EBI_SMI64_128MB_SIZE;
		printk(KERN_ERR "use default  :  smisize=%d\n", smi_sz);
	}
#else
	if (32 == smi_sz) {
		mi->bank[0].size = (84*1024*1024);
	} else if (64 == smi_sz){
		mi->bank[0].size = SMI64_MSM_LINUX_SIZE;	//(101*1024*1024);
	} else {
		printk(KERN_ERR "can not get smi size\n");

		/*Give a default value when not get smi size*/
		smi_sz = 64;
		mi->bank[0].size = SMI64_MSM_LINUX_SIZE;	//(101*1024*1024);
		printk(KERN_ERR "use default  :  smisize=%d\n", smi_sz);
	}
#endif
	printk("sapphire_fixup:bank size=0x%lx\n", mi->bank[0].size);
}

static void __init sapphire_map_io(void)
{
	msm_map_common_io();
	iotable_init(sapphire_io_desc, ARRAY_SIZE(sapphire_io_desc));
	msm_clock_init();
}

MACHINE_START(SAPPHIRE, "sapphire")
/* Maintainer: Brian Swetland <swetland@google.com> */
#ifdef CONFIG_MSM_DEBUG_UART
	.phys_io        = MSM_DEBUG_UART_PHYS,
	.io_pg_offst    = ((MSM_DEBUG_UART_BASE) >> 18) & 0xfffc,
#endif
#if defined(CONFIG_MSM_AMSS_SUPPORT_256MB_EBI1)
	.boot_params    = 0x19200100,
#else
	.boot_params    = 0x10000100,
#endif
	.fixup          = sapphire_fixup,
	.map_io         = sapphire_map_io,
	.init_irq       = sapphire_init_irq,
	.init_machine   = sapphire_init,
	.timer          = &msm_timer,
MACHINE_END
