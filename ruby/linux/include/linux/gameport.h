#ifndef _GAMEPORT_H
#define _GAMEPORT_H

/*
 * gameport.h  Version 0.1
 *
 * Copyright (C) 1999 Vojtech Pavlik
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
 * e-mail - mail your message to <vojtech@ucw.cz>, or by paper mail:
 * Vojtech Pavlik, Ucitelska 1576, Prague 8, 182 00 Czech Republic
 */

#include <asm/io.h>

struct gameport;

struct gameport {

	void *private;
	void *driver;

	int number;

	int io;
	int size;
	int speed;
	int type;
	struct pci_dev *pci;

	void (*trigger)(struct gameport *);
	unsigned char (*read)(struct gameport *);
	int (*cooked_read)(struct gameport *, int *, int *);	
	int (*set_mode)(struct gameport *, int);
	int (*open)(struct gameport *);
	void (*close)(struct gameport *);

	struct gameport_dev *dev;

	struct gameport *next;
};

struct gameport_dev {

	void *private;

	void (*connect)(struct gameport *, struct gameport_dev *dev);
	void (*disconnect)(struct gameport *);

	struct gameport_dev *next;
};

int gameport_open(struct gameport *gameport, struct gameport_dev *dev);
void gameport_close(struct gameport *gameport);
void gameport_rescan(struct gameport *gameport);

void gameport_register_port(struct gameport *gameport);
void gameport_unregister_port(struct gameport *gameport);
void gameport_register_device(struct gameport_dev *dev);
void gameport_unregister_device(struct gameport_dev *dev);

#define GAMEPORT_MODE_DISABLED		0
#define GAMEPORT_MODE_RAW		1
#define GAMEPORT_MODE_COOKED		2

#define GAMEPORT_ISA       0
#define GAMEPORT_PNP       1
#define GAMEPORT_EXT       2

static __inline__ void gameport_trigger(struct gameport *gameport)
{
#ifdef CONFIG_GAMEPORT_OTHER
	if (gameport->trigger)
		gameport->trigger(gameport);
	else
#endif
	outb(0xff, gameport->io);
}

static __inline__ unsigned char gameport_read(struct gameport *gameport)
{
#ifdef CONFIG_GAMEPORT_OTHER
	if (gameport->read)
		gameport->read(gameport);
	else
#endif
	return inb(gameport->io);
}

static __inline__ int gameport_cooked_read(struct gameport *gameport, int *axes, int *buttons)
{
	if (gameport->cooked_read)
		return gameport->cooked_read(gameport, axes, buttons);
	else
		return -1;
}

static __inline__ int gameport_set_mode(struct gameport *gameport, int mode)
{
	if (gameport->set_mode)
		return gameport->set_mode(gameport, mode);
	else
		return -(mode != GAMEPORT_MODE_RAW);
}

static __inline__ int gameport_time(struct gameport *gameport, int time)
{
	return (time * gameport->speed) / 1000;
}

static __inline__ void wait_ms(unsigned int ms)
{
	current->state = TASK_UNINTERRUPTIBLE;
	schedule_timeout(1 + ms * HZ / 1000);
}

#endif
