/*
 * $Id$
 *
 *  Copyright (c) 1996-2000 Vojtech Pavlik
 *
 *  Sponsored by SuSE
 */

/*
 * Analog joystick and gamepad driver for Linux
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or 
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 * 
 * Should you need to contact me, the author, you can do so either by
 * e-mail - mail your message to <vojtech@suse.cz>, or by paper mail:
 * Vojtech Pavlik, Ucitelska 1576, Prague 8, 182 00 Czech Republic
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/malloc.h>
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/gameport.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@suse.cz>");

/*
 * Option parsing.
 */

MODULE_PARM(js,"1-16s");

#define ANALOG_PORTS		16

static char *js[ANALOG_PORTS];
static int analog_options[ANALOG_PORTS];

/*
 * Times, feature definitions.
 */

#define ANALOG_RUDDER		0x04
#define ANALOG_THROTTLE		0x08
#define ANALOG_AXES_STD		0x0f
#define ANALOG_BTNS_STD		0xf0

#define ANALOG_BTNS_CHF		0x0100
#define ANALOG_HAT1_CHF		0x0200
#define ANALOG_HAT2_CHF		0x0400
#define ANALOG_ANY_CHF		0x0700
#define ANALOG_HAT_FCS		0x0800
#define ANALOG_HATS_ALL		0x0e00
#define ANALOG_BTN_TL		0x1000
#define ANALOG_BTN_TR		0x2000
#define ANALOG_BTN_TL2		0x4000
#define ANALOG_BTN_TR2		0x8000
#define ANALOG_BTNS_TLR		0x3000
#define ANALOG_BTNS_TLR2	0xc000
#define ANALOG_BTNS_GAMEPAD	0xf000
#define ANALOG_EXTENSIONS	0xff00

#define ANALOG_GAMEPAD		0x10000
#define ANALOG_SAITEK		0x20000

#define ANALOG_MAX_TIME		3	/* 3 ms */
#define ANALOG_LOOP_TIME	1500	/* 1.5 * loop */
#define ANALOG_REFRESH_TIME	HZ/100	/* 10 ms */
#define ANALOG_AXIS_TIME	2	/* 2 * refresh */
#define ANALOG_INIT_RETRIES	8	/* 8 times */
#define ANALOG_RESOLUTION	12	/* 12 bits */
#define ANALOG_FUZZ		16	/* 4 bit gauss */

#define ANALOG_MAX_NAME_LENGTH  128

static struct {
	int x;
	int y;
} analog_hat_to_axis[] = {{ 0, 0}, { 0,-1}, { 1, 0}, { 0, 1}, {-1, 0}};

static int analog_axes[] = { ABS_X, ABS_Y, ABS_RUDDER, ABS_THROTTLE };
static int analog_hats[] = { ABS_HAT0X, ABS_HAT0Y, ABS_HAT1X, ABS_HAT1Y, ABS_HAT2X, ABS_HAT2Y };
static int analog_exts[] = { ANALOG_HAT1_CHF, ANALOG_HAT2_CHF, ANALOG_HAT_FCS };
static int analog_pad_btn[] = { BTN_A, BTN_B, BTN_C, BTN_X, BTN_TL2, BTN_TR2, BTN_Y, BTN_Z, BTN_TL, BTN_TR };
static int analog_joy_btn[] = { BTN_TRIGGER, BTN_THUMB, BTN_TOP, BTN_TOP2, BTN_BASE, BTN_BASE2,
				BTN_BASE3, BTN_BASE4, BTN_BASE5, BTN_THUMB2 };

static int analog_saitek_axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_THROTTLE };
static int analog_saitek_btn[] = { BTN_TRIGGER, BTN_THUMB, BTN_TOP, BTN_TOP2, BTN_BASE, BTN_BASE2,
					BTN_BASE3, BTN_BASE4, BTN_BASE5, BTN_THUMB2, BTN_A, BTN_B, BTN_C, BTN_X,
					BTN_Y, BTN_Z };

struct analog {
	struct input_dev dev;
	int mask;
	int *buttons;
	int *axes;
	char name[ANALOG_MAX_NAME_LENGTH];
};

struct analog_port {
	struct gameport *gameport;
	struct timer_list timer;
	struct analog analog[2];
	char mask;
	int bads;
	int reads;
	int speed;
	int loop;
	int timeout;
	int fuzz;
	int cooked;
	int axes[4];
	int buttons;
	int initial[4];
	int used;
	int axtime;
};

/*
 * Time macros.
 */

#ifdef __i386__
#ifdef CONFIG_X86_TSC
#define GET_TIME(x)	__asm__ __volatile__ ( "rdtsc" : "=a" (x) : : "dx" )
#define DELTA(x,y)	((y)-(x))
#define TIME_NAME "TSC"
#else
#define GET_TIME(x)	do { outb(0, 0x43); x = inb(0x40); x |= inb(0x40) << 8; } while (0)
#define DELTA(x,y)	((x)-(y)+((x)<(y)?1193180L/HZ:0))
#define TIME_NAME "PIT"
#endif
#elif __alpha__
#define GET_TIME(x)	__asm__ __volatile__ ( "rpcc %0" : "=r" (x) )
#define DELTA(x,y)	((y)-(x))
#define TIME_NAME "PCC"
#endif

#ifndef GET_TIME
#define FAKE_TIME
static unsigned long analog_faketime = 0;
#define GET_TIME(x)     do { x = analog_faketime++; } while(0)
#define DELTA(x,y)	((y)-(x))
#define TIME_NAME "Unreliable"
#endif

/*
 * analog_decode() decodes analog joystick data and reports input events.
 */

static void analog_decode(struct analog *analog, int *axes, int *initial, int buttons)
{
	struct input_dev *dev = &analog->dev;
	int i, j;
	int hat[3] = {0, 0, 0};

	if (analog->mask & ANALOG_ANY_CHF)
		switch (buttons & 0xf) {
			case 0x5: buttons = 0x10; break;
			case 0x9: buttons = 0x20; break;
			case 0x3: hat[0]++;
			case 0x7: hat[0]++;
			case 0xb: hat[0]++;
			case 0xf: hat[0]++; buttons = 0; break;
			case 0xc: hat[1]++;
			case 0x6: hat[1]++;
			case 0xa: hat[1]++;
			case 0xe: hat[1]++; buttons = 0; break;
		}

	for (i = j = 0; i < 6; i++)
		if (analog->mask & (0x10 << i))
			input_report_key(dev, analog->buttons[j++], (buttons >> i) & 1);

	if (analog->mask & ANALOG_BTN_TL)
		input_report_key(dev, analog->buttons[6], axes[2] < (initial[2] >> 1));
	if (analog->mask & ANALOG_BTN_TR)
		input_report_key(dev, analog->buttons[7], axes[3] < (initial[3] >> 1));
	if (analog->mask & ANALOG_BTN_TL2)
		input_report_key(dev, analog->buttons[8], axes[2] > (initial[2] + (initial[2] >> 1)));
	if (analog->mask & ANALOG_BTN_TR2)
		input_report_key(dev, analog->buttons[9], axes[3] > (initial[3] + (initial[3] >> 1)));

	if (analog->mask & ANALOG_SAITEK)
		for (i = 0; i < 16; i++)
			input_report_key(dev, analog_saitek_btn[i], (buttons >> i) & 1);

	if (analog->mask & ANALOG_HAT_FCS)
		for (i = 0; i < 4; i++)
			if (axes[3] < ((initial[3] * ((i << 1) + 1)) >> 3)) {
				hat[2] = i + 1;
				break;
			}

	for (i = j = 0; i < 4; i++)
		if (analog->mask & (1 << i))
			input_report_abs(dev, analog->axes[j++], axes[i]);

	for (i = j = 0; i < 3; i++)
		if (analog->mask & analog_exts[i]) {
			input_report_abs(dev, analog_hats[j++], analog_hat_to_axis[hat[i]].x);
			input_report_abs(dev, analog_hats[j++], analog_hat_to_axis[hat[i]].y);
		}
}

/*
 * analog_cooked_read() reads analog joystick data.
 */

static int analog_cooked_read(struct analog_port *port)
{
	struct gameport *gameport = port->gameport;
	unsigned int time[4], start, loop, now;
	unsigned char data[4], this, last;
	unsigned long flags;
	int i, j;
	
	__save_flags(flags);
	__cli();
	gameport_trigger(gameport);
	GET_TIME(now);
	__restore_flags(flags);

	start = now;
	this = port->mask;
	i = 0;

	do {
		loop = now;
		last = this;

		__cli();
		this = gameport_read(gameport) & port->mask;
		GET_TIME(now);
		__restore_flags(flags);

		if ((last ^ this) && (DELTA(loop, now) < port->loop)) {
			data[i] = last ^ this;
			time[i] = now;
			i++;
		}

	} while (this && (i < 4) && (DELTA(start, now) < port->timeout));

	this <<= 4;

	for (--i; i >= 0; i--) {
		this |= data[i];
		for (j = 0; j < 4; j++)
			if (data[i] & (1 << j))
				port->axes[j] = (DELTA(start, time[i]) << ANALOG_RESOLUTION) / port->speed;
	}

	return -(this != port->mask);
}

static int analog_button_read(struct analog_port *port)
{
	port->buttons = (~gameport_read(port->gameport) >> 4) & 0xf;
	return 0;
}

static int analog_saitek_read(struct analog_port *port)
{
	unsigned char u;
	int i = 0;

	port->buttons = 0;
	u = (~gameport_read(port->gameport) >> 4) & 0xf;

	while (u && i < 16) {

		port->buttons |= 1 << u;
		udelay(310);
		gameport_trigger(port->gameport);
		udelay(70);
		u = (~gameport_read(port->gameport) >> 4) & 0xf;
		i++;
	}
	return 0;
}

/*
 * analog_timer() repeatedly polls the Analog joysticks.
 */

static void analog_timer(unsigned long data)
{
	struct analog_port *port = (void *) data;
	int i;

	if (port->cooked) {
		port->bads -= gameport_cooked_read(port->gameport, port->axes, &port->buttons);
		port->reads++;
	} else {
		if (~port->analog[0].mask & ANALOG_SAITEK)
			analog_button_read(port);
		if (!port->axtime--) {
			port->bads -= analog_cooked_read(port);
			port->reads++;
			port->axtime = ANALOG_AXIS_TIME;
			if (port->analog[0].mask & ANALOG_SAITEK)
				analog_saitek_read(port);
		}
	}

	for (i = 0; i < 2; i++) 
		if (port->analog[i].mask)
			analog_decode(port->analog + i, port->axes, port->initial, port->buttons);

	mod_timer(&port->timer, jiffies + ANALOG_REFRESH_TIME);
}

/*
 * analog_open() is a callback from the input open routine.
 */

static int analog_open(struct input_dev *dev)
{
	struct analog_port *port = dev->private;
	if (!port->used++)
		mod_timer(&port->timer, jiffies + ANALOG_REFRESH_TIME);	
	return 0;
}

/*
 * analog_close() is a callback from the input close routine.
 */

static void analog_close(struct input_dev *dev)
{
	struct analog_port *port = dev->private;
	if (!--port->used)
		del_timer(&port->timer);
}

/*
 * analog_calibrate_timer() calibrates the timer and computes loop
 * and timeout values for a joystick port.
 */

static void analog_calibrate_timer(struct analog_port *port)
{
	struct gameport *gameport = port->gameport;
	unsigned int i, t, tx, t1, t2, t3;
	unsigned long flags;

	save_flags(flags);
	cli();
	GET_TIME(t1);
#ifdef FAKE_TIME
	analog_faketime += 830;
#endif
	udelay(1000);
	GET_TIME(t2);
	GET_TIME(t3);
	restore_flags(flags);

	port->speed = DELTA(t1, t2) - DELTA(t2, t3);

	tx = ~0;

	for (i = 0; i < 50; i++) {
		save_flags(flags);
		cli();
		GET_TIME(t1);
		for (t = 0; t < 50; t++) { gameport_read(gameport); GET_TIME(t2); }
		GET_TIME(t3);
		restore_flags(flags);
		udelay(i);
		t = DELTA(t1, t2) - DELTA(t2, t3);
		if (t < tx) tx = t;
	}

        port->loop = (ANALOG_LOOP_TIME * tx) / 50000;
	port->timeout = ANALOG_MAX_TIME * port->speed;
}

/*
 * analog_name() constructs a name for an analog joystick.
 */

static void analog_name(struct analog *analog)
{
	sprintf(analog->name, "Analog %d-axis %d-button", 
		hweight8(analog->mask & ANALOG_AXES_STD),
		hweight8(analog->mask & ANALOG_BTNS_STD) + !!(analog->mask & ANALOG_BTNS_CHF) * 2 +
		hweight16(analog->mask & ANALOG_BTNS_GAMEPAD));

	if (analog->mask & ANALOG_HATS_ALL)
		sprintf(analog->name, "%s %d-hat",
			analog->name, hweight16(analog->mask & ANALOG_HATS_ALL));

	if (analog->mask & ANALOG_HAT_FCS) strcat(analog->name, " FCS");
	if (analog->mask & ANALOG_ANY_CHF) strcat(analog->name, " CHF");
	
	strcat(analog->name, (analog->mask & ANALOG_GAMEPAD) ? " gamepad": " joystick");
}

/*
 * analog_init_device()
 */

static void analog_init_device(struct analog_port *port, struct analog *analog, int index)
{
	int i, j, t, x, y;

	analog_name(analog);

	analog->buttons = (analog->mask & ANALOG_GAMEPAD) ? analog_pad_btn : analog_joy_btn;
	analog->axes = (analog->mask & ANALOG_SAITEK) ? analog_saitek_axes : analog_axes;

	analog->dev.open = analog_open;
	analog->dev.close = analog_close;
	analog->dev.private = port;
	analog->dev.evbit[0] = BIT(EV_KEY) | BIT(EV_ABS);
	
	for (i = j = 0; i < 4; i++)
		if (analog->mask & (1 << i)) {
			
			t = analog->axes[j];
			x = port->axes[i];
			y = (x >> 3);
	
			set_bit(t, analog->dev.absbit);

			if ((i == 2 || i == 3) && (t == ABS_THROTTLE || t == ABS_RUDDER)) {
				x = (port->axes[0] + port->axes[1]) >> 1;
				y = 0;
			}

			analog->dev.absmax[t] = (x << 1) - (x >> 3);
			analog->dev.absmin[t] = (x >> 3);
			analog->dev.absfuzz[t] = port->fuzz;
			analog->dev.absflat[t] = y;

			j++;
		}

	for (i = j = 0; i < 3; i++) 
		if (analog->mask & analog_exts[i]) 
			for (x = 0; x < 2; x++) {
				t = analog_hats[j++];
				set_bit(t, analog->dev.absbit);
				analog->dev.absmax[t] = 1;
				analog->dev.absmin[t] = -1;
			}

	for (i = j = 0; i < 4; i++)
		if (analog->mask & (0x10 << i))
			set_bit(analog->buttons[j++], analog->dev.keybit);

	if (analog->mask & ANALOG_BTNS_CHF) {
		set_bit(analog->buttons[j++], analog->dev.keybit);
		set_bit(analog->buttons[j++], analog->dev.keybit);
	}

	for (i = 0; i < 4; i++)
		if (analog->mask & (ANALOG_BTN_TL << i))
			set_bit(analog->buttons[i + 6], analog->dev.keybit);

	if (analog->mask & ANALOG_SAITEK)
		for (i = 0; i < 16; i++)
			set_bit(analog_saitek_btn[i], analog->dev.keybit);

	analog_decode(analog, port->axes, port->initial, port->buttons);

	input_register_device(&analog->dev);

	printk(KERN_INFO "input%d: %s at gameport%d.%d",
		analog->dev.number, analog->name, port->gameport->number, index);

	if (port->cooked)
		printk(" [ADC port]\n");
	else
		printk(" ["TIME_NAME" timer, %d %sHz clock, %d ns res]\n",
		port->speed > 10000 ? (port->speed + 800) / 1000 : port->speed,
		port->speed > 10000 ? "M" : "k",
		((((port->loop * 1000000) / port->speed) * 1000) / ANALOG_LOOP_TIME));
}

/*
 * analog_init_devices() sets up device-specific values and registers the input devices.
 */

static int analog_init_masks(struct analog_port *port)
{
	int i;
	struct analog *analog = port->analog;
	int max[4];

	if (!port->mask)
		return -1;

	if ((port->mask & 3) != 3 && port->mask != 0xc) {
		printk(KERN_WARNING "analog.c: Unknown joystick device found  "
			"(data=%#x, gameport%d), probably not analog joystick.\n",
			port->mask, port->gameport->number);
		return -1;
	}

	i = port->gameport->number < ANALOG_PORTS ? analog_options[port->gameport->number] : 0xff;

	analog[0].mask = i & 0xfffff;

	analog[0].mask &= ~(ANALOG_AXES_STD | ANALOG_HAT_FCS | ANALOG_BTNS_GAMEPAD)
			| port->mask | ((port->mask << 8) & ANALOG_HAT_FCS)
			| ((port->mask << 10) & ANALOG_BTNS_TLR) | ((port->mask << 12) & ANALOG_BTNS_TLR2);

	analog[0].mask &= (analog[0].mask & ANALOG_SAITEK) ? (ANALOG_SAITEK | ANALOG_AXES_STD) : ~0;

	analog[0].mask &= ~(ANALOG_THROTTLE | ANALOG_BTN_TR | ANALOG_BTN_TR2)
			| ((~analog[0].mask & ANALOG_HAT_FCS) >> 8)
			| ((~analog[0].mask & ANALOG_HAT_FCS) << 2)
			| ((~analog[0].mask & ANALOG_HAT_FCS) << 4);

	analog[0].mask &= ~(ANALOG_THROTTLE | ANALOG_RUDDER)
			| (((~analog[0].mask & ANALOG_BTNS_TLR ) >> 10)
			&  ((~analog[0].mask & ANALOG_BTNS_TLR2) >> 12));

	analog[1].mask = ((i >> 20) & 0xff) | ((i >> 12) & 0xf0000);

	analog[1].mask &= (analog[0].mask & ANALOG_EXTENSIONS & ~ANALOG_GAMEPAD) ? 0
			: (((ANALOG_BTNS_STD | port->mask) & ~analog[0].mask) | ANALOG_GAMEPAD);

	if (port->cooked) {

		for (i = 0; i < 4; i++) max[i] = port->axes[i] << 1;

		if ((analog[0].mask & 0x7) == 0x7) max[2] = (max[0] + max[1]) >> 1;
		if ((analog[0].mask & 0xb) == 0xb) max[3] = (max[0] + max[1]) >> 1;
		if ((analog[0].mask & ANALOG_BTN_TL) && !(analog[0].mask & ANALOG_BTN_TL2)) max[2] >>= 1;
		if ((analog[0].mask & ANALOG_BTN_TR) && !(analog[0].mask & ANALOG_BTN_TR2)) max[3] >>= 1;
		if ((analog[0].mask & ANALOG_HAT_FCS)) max[3] >>= 1;

		gameport_calibrate(port->gameport, port->axes, max);
	}
		
	for (i = 0; i < 4; i++) 
		port->initial[i] = port->axes[i];

	return -!(analog[0].mask || analog[1].mask);	
}

static void analog_connect(struct gameport *gameport, struct gameport_dev *dev)
{
	struct analog_port *port;
	int i;

	if (!(port = kmalloc(sizeof(struct analog_port), GFP_KERNEL)))
		return;
	memset(port, 0, sizeof(struct analog_port));

	gameport->private = port;

	port->gameport = gameport;
	init_timer(&port->timer);
	port->timer.data = (long) port;
	port->timer.function = analog_timer;

	if (((port->gameport->number < ANALOG_PORTS) &&
	     (analog_options[port->gameport->number] & ANALOG_SAITEK)) ||
	      gameport_open(gameport, dev, GAMEPORT_MODE_COOKED)) {
		if (gameport_open(gameport, dev, GAMEPORT_MODE_RAW)) {
			kfree(port);
			return;
		}
	} else port->cooked = 1;

	if (port->cooked) {
		for (i = 0; i < ANALOG_INIT_RETRIES; i++)
			if (!gameport_cooked_read(gameport, port->axes, &port->buttons))
				break;
		for (i = 0; i < 4; i++)
			if (port->axes[i] != -1) port->mask |= 1 << i;
		port->fuzz = gameport->fuzz;
	} else {
		analog_calibrate_timer(port);
		gameport_trigger(gameport);
		port->mask = gameport_read(gameport);
		wait_ms(ANALOG_MAX_TIME);
		port->mask = (gameport_read(gameport) ^ port->mask) & port->mask & 0xf;
		for (i = 0; i < ANALOG_INIT_RETRIES; i++)
			if (!analog_cooked_read(port))
				break;
		port->fuzz = ANALOG_FUZZ;
	}

	if (analog_init_masks(port)) {
		gameport_close(gameport);
		kfree(port);
		return;
	}

	for (i = 0; i < 2; i++)
		if (port->analog[i].mask)
			analog_init_device(port, port->analog + i, i);
}

static void analog_disconnect(struct gameport *gameport)
{
	int i;

	struct analog_port *port = gameport->private;
	for (i = 0; i < 2; i++)
		if (port->analog[i].mask)
			input_unregister_device(&port->analog[i].dev);
	gameport_close(gameport);
	printk(KERN_INFO "analog.c: %d out of %d reads (%d%%) on gameport%d failed\n",
		port->bads, port->reads, port->reads ? (port->bads * 100 / port->reads) : 0,
		port->gameport->number);
	kfree(port);
}

struct analog_types {
	char *name;
	int value;
};

struct analog_types analog_types[] = {
	{ "none",	0x00000000 },
	{ "auto",	0x000000ff },
	{ "2btn",	0x0000003f },
	{ "4btn",	0x000000ff },
	{ "y-joy",	0x0cc00033 },
	{ "y-pad",	0x1cc10033 },
	{ "fcs",	0x000008f7 },
	{ "chf",	0x000002ff },
	{ "fullchf",	0x000007ff },
	{ "gamepad",	0x000130f3 },
	{ "gamepad8",	0x0001f0f3 },
	{ "saitek",	0x000200ff },
	{ NULL, 0 }
};

static void analog_parse_options(void)
{
	int i, j;
	char *end;

	for (i = 0; i < ANALOG_PORTS && js[i]; i++) {

		for (j = 0; analog_types[j].name; j++)
			if (!strcmp(analog_types[j].name, js[i])) {
				analog_options[i] = analog_types[j].value;
				break;
			} 
		if (analog_types[j].name) continue;

		analog_options[i] = simple_strtoul(js[i], &end, 0);
		if (end != js[i]) continue;

		analog_options[i] = 0xff;
		if (!strlen(js[i])) continue;

		printk(KERN_WARNING "analog.c: Bad config for port %d - \"%s\"\n", i, js[i]);
	}

	for (; i < ANALOG_PORTS; i++)
		analog_options[i] = 0xff;
}

/*
 * The gameport device structure.
 */

static struct gameport_dev analog_dev = {
	connect:	analog_connect,
	disconnect:	analog_disconnect,
};

#ifndef MODULE
static int __init analog_setup(char *str)
{
	char *s = str;
	int i = 0;

	if (!str || !*str) return 0;

	while ((str = s) && (i < ANALOG_PORTS)) {
		if ((s = strchr(str,','))) *s++ = 0;
		js[i++] = str;
	}

	return 1;
}
__setup("js=", analog_setup);
#endif

int __init analog_init(void)
{
	analog_parse_options();
	gameport_register_device(&analog_dev);
	return 0;
}

void __exit analog_exit(void)
{
	gameport_unregister_device(&analog_dev);
}

module_init(analog_init);
module_exit(analog_exit);
