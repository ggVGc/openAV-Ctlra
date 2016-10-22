/*
 * Copyright (c) 2016, OpenAV Productions,
 * Harry van Haaren <harryhaaren@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef OPENAV_CTLR_NI_MASCHINE
#define OPENAV_CTLR_NI_MASCHINE

/* Uncomment to see debug output */
//#define NI_MASCHINE_DEBUG

#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

#include "device_impl.h"

#define NPADS                   16
#define NI_VENDOR               0x17cc
#define NI_MASCHINE_MIKRO_MK2   0x1200
#define KERNEL_LENGTH           8
#define PAD_SENSITIVITY         250

#define PAD_PRESS_BRIGHTNESS    (0.9999f)
#define PAD_RELEASE_BRIGHTNESS  (0.25f)

typedef enum {
	MASCHINE_BUTTON_DOWN,
	MASCHINE_BUTTON_UP,
	MASCHINE_ENCODER_TURN
} ni_maschine_event_type_t;

typedef enum {
	MASCHINE_BTN_RESTART,
	MASCHINE_BTN_STEP_LEFT,
	MASCHINE_BTN_STEP_RIGHT,
	MASCHINE_BTN_GRID,
	MASCHINE_BTN_PLAY,
	MASCHINE_BTN_REC,
	MASCHINE_BTN_ERASE,
	MASCHINE_BTN_SHIFT,

	MASCHINE_BTN_BROWSE,
	MASCHINE_BTN_SAMPLING,
	MASCHINE_BTN_GROUP,
	MASCHINE_BTN_NOTE_REPEAT,
	MASCHINE_BTN_ENCODER,

	MASCHINE_BTN_F1,
	MASCHINE_BTN_F2,
	MASCHINE_BTN_F3,
	MASCHINE_BTN_MAIN,
	MASCHINE_BTN_NAV,
	MASCHINE_BTN_NAV_LEFT,
	MASCHINE_BTN_NAV_RIGHT,
	MASCHINE_BTN_ENTER,

	MASCHINE_BTN_SCENE,
	MASCHINE_BTN_PATTERN,
	MASCHINE_BTN_PAD_MODE,
	MASCHINE_BTN_VIEW,
	MASCHINE_BTN_DUPLICATE,
	MASCHINE_BTN_SELECT,
	MASCHINE_BTN_SOLO,
	MASCHINE_BTN_MUTE,

	/* not technically a button, but close enough */
	MASCHINE_BTN_ENCODER_TURN,

	MASCHINE_BTN_COUNT,
} ni_maschine_buttons_t;

static const int mikro_bitfield_button_map[4][8] = {
	{
		MASCHINE_BTN_RESTART,
		MASCHINE_BTN_STEP_LEFT,
		MASCHINE_BTN_STEP_RIGHT,
		MASCHINE_BTN_GRID,
		MASCHINE_BTN_PLAY,
		MASCHINE_BTN_REC,
		MASCHINE_BTN_ERASE,
		MASCHINE_BTN_SHIFT
	},

	{
		MASCHINE_BTN_BROWSE,
		MASCHINE_BTN_SAMPLING,
		MASCHINE_BTN_GROUP,
		MASCHINE_BTN_NOTE_REPEAT,
		MASCHINE_BTN_ENCODER,
		0,
		0,
		0
	},

	{
		MASCHINE_BTN_F1,
		MASCHINE_BTN_F2,
		MASCHINE_BTN_F3,
		MASCHINE_BTN_MAIN,
		MASCHINE_BTN_NAV,
		MASCHINE_BTN_NAV_LEFT,
		MASCHINE_BTN_NAV_RIGHT,
		MASCHINE_BTN_ENTER
	},

	{
		MASCHINE_BTN_SCENE,
		MASCHINE_BTN_PATTERN,
		MASCHINE_BTN_PAD_MODE,
		MASCHINE_BTN_VIEW,
		MASCHINE_BTN_DUPLICATE,
		MASCHINE_BTN_SELECT,
		MASCHINE_BTN_SOLO,
		MASCHINE_BTN_MUTE
	}
};


struct ni_maschine_t {
	struct ctlr_dev_t base;
	/* File descriptor to read/write USB HID messages */
	int fd;
	/* State of the pads */
	uint16_t pads[16];
	/* Initialized buffers for light and button messages */
	uint8_t light_buf[79];
	uint8_t button_buf[5];
	/* Pressure filtering for note-onset detection */
	uint8_t  pad_idx[NPADS];
	uint16_t pad_pressures[NPADS*KERNEL_LENGTH];
	uint16_t pad_avg[NPADS];
};

static void ni_maschine_led_flush(struct ni_maschine_t *dev)
{
	dev->light_buf[0] = 128;
	const uint32_t size = sizeof(dev->light_buf);
	int ret = write(dev->fd, dev->light_buf, size);
	if(ret != size) {
#ifdef NI_MASCHINE_DEBUG
		printf("%s : write error\n", __func__);
#endif
	}
}

struct ni_maschine_rgb {
	uint8_t r, g, b;
};

static void
ni_maschine_pad_light_set(struct ni_maschine_t *dev, int pad, uint32_t col,
			  float bright)
{
	struct ni_maschine_rgb *lights;
	lights = (struct ni_maschine_rgb *) &dev->light_buf[31 + (pad * 3)];

	bright *= .5;
	lights->r = lrintf(bright * ((col >> 16) & 0xFF));
	lights->g = lrintf(bright * ((col >>  8) & 0xFF));
	lights->b = lrintf(bright * ((col      ) & 0xFF));
}

static void
ni_maschine_set_brightness_contrast(struct ni_maschine_t *dev,
				    uint8_t brightness,
				    uint8_t contrast)
{
	uint8_t msg[11] = {
		0xF8,
		0x80,

		0x00,
		0x40,

		0x00,
		0x01,
		0x00,
		0x01,

		brightness,
		contrast,

		0x00
	};
	ioctl(dev->fd, HIDIOCSFEATURE(11), msg);
}

static void
button_dispatch(struct ni_maschine_t *dev, uint8_t *data)
{
	int i, off, btn_id, diff;

	for (i = 0; i < 4; i++) {
		diff = data[i] ^ dev->button_buf[i];

		for (; !!diff; diff >>= off) {
			off = ffs(diff);
			btn_id = mikro_bitfield_button_map[i][8 - off];

			int press = (data[i] & (1 << (off - 1))) > 0;

			printf("btn id %d, press %d\n", btn_id, press);
		}

		dev->button_buf[i] = data[i];
	}

	if (dev->button_buf[4] > 15) {
		dev->button_buf[4] = data[4];
		return;
	}

	if (dev->button_buf[4] == data[4])
		return;

	if (((dev->button_buf[4] + 1) & 0xF) == data[4]) {
		printf("encoder turn:  1\n");
	}
	else {
		printf("encoder turn: -1\n");
	}

	dev->button_buf[4] = data[4];
}


static int32_t ni_maschine_light_set(struct ctlr_dev_t *base,
				  uint32_t light_id, uint32_t status)
{
	struct ni_maschine_t *dev = (struct ni_maschine_t *)base;
	switch(light_id)
	{
	case 1 ... 8: /* fallthrough */
	case 10 ... 30:
		dev->light_buf[light_id] = (status >> 24) & 0x7F;
		break;
	case 9:
		dev->light_buf[light_id  ] = ((status >> 16) & 0xFF) / 2;
		dev->light_buf[light_id+1] = ((status >>  8) & 0xFF) / 2;
		dev->light_buf[light_id+2] = ((status >>  0) & 0xFF) / 2;
		break;
	case 31 ... 47:
		dev->light_buf[light_id  ] = ((status >> 16) & 0xFF) / 2;
		dev->light_buf[light_id+1] = ((status >>  8) & 0xFF) / 2;
		dev->light_buf[light_id+2] = ((status >>  0) & 0xFF) / 2;
		break;
	default:
		printf("light default hit\n");
		break;
	}
	ni_maschine_led_flush(dev);

	return 0;
}

static uint32_t ni_maschine_poll(struct ctlr_dev_t *dev);
static int32_t ni_maschine_disconnect(struct ctlr_dev_t *dev);

struct ctlr_dev_t *ni_maschine_connect(void *future)
{
	(void)future;
	struct ni_maschine_t *dev = calloc(1, sizeof(struct ni_maschine_t));
	if(!dev)
		goto fail;

	int fd, i, res, found = 0;
	char buf[256];
	struct hidraw_devinfo info;
	uint8_t *l = dev->light_buf;

	/* Scan for the USB HID raw device, probe for NI Maschine Mikro */
	for (i = 0; i < 64; i++) {
		const char *device = "/dev/hidraw";
		snprintf(buf, sizeof(buf), "%s%d", device, i);
		fd = open(buf, O_RDWR);
		if(fd < 0)
			continue;

		memset(&info, 0x0, sizeof(info));
		res = ioctl(fd, HIDIOCGRAWINFO, &info);

		if (res < 0) // Error with HIDIOCGRAWINFO
			goto fail;

		if (info.vendor  == NI_VENDOR &&
		   info.product == NI_MASCHINE_MIKRO_MK2) {
			dev->fd = fd;
			found = 1;
			break;
		}

		close(fd);
		/* continue searching next HID dev */
	}

	if(!found)
		goto fail;

	/* Initialize light buffer contents */
	l[0] = 0x80;
	for(int i = 0; i <= 30; i++)
		l[i] = 0;
	for(int i = 31; i <= 78; i++)
		l[i] = 0;
	l[19] = 0;

	/* Initialize button contents */
	dev->button_buf[4] = 16;

#ifdef NI_MASCHINE_DEBUG
	printf("Maschine connected at %s\n", buf);
#endif
	for(int i = 0; i <= 30; i++)
		l[i] = 5;
	for(int i = 0; i < 16; i++)
		ni_maschine_pad_light_set(dev, i, 0xF0F0F0,
					  PAD_RELEASE_BRIGHTNESS);
	ni_maschine_led_flush(dev);

	ni_maschine_set_brightness_contrast(dev, 20, 20);

	/* Assign instance callbacks */
	dev->base.poll = ni_maschine_poll;
	dev->base.disconnect = ni_maschine_disconnect;
	dev->base.light_set = ni_maschine_light_set;

	return (struct ctlr_dev_t *)dev;
fail:
	if(dev->fd)
		close(dev->fd);
	if(dev)
		free(dev);
	return 0;
}

static int
unpack_pads(struct ni_maschine_t *dev, uint8_t *data, uint16_t *pads)
{
	uint16_t new_val;
	int i;
	int changed = 0;

	for (i = 0; i < NPADS; i++) {
		uint16_t data1_mask = (data[1] & 0x0F);
		new_val = data[0] | ( data1_mask << 8);
		data += 2;

		uint8_t idx = dev->pad_idx[i]++ & (KERNEL_LENGTH-1);

		uint16_t total = 0;
		for(int j = 0; j < KERNEL_LENGTH; j++)
		{
			total += dev->pad_pressures[i*KERNEL_LENGTH + j];
		}
		dev->pad_avg[i] = total / KERNEL_LENGTH;

		dev->pad_pressures[i*KERNEL_LENGTH + idx] = new_val;

		if(dev->pad_avg[i] > PAD_SENSITIVITY && pads[i] == 0) {
#ifdef NI_MASCHINE_DEBUG
			printf("%d pressed\n", i);
#endif
			pads[i] = 2000;
			changed = 1;
		}
		else if(dev->pad_avg[i] < PAD_SENSITIVITY && pads[i] > 0) {
#ifdef NI_MASCHINE_DEBUG
			printf("%d release\n", i);
#endif
			pads[i] = 0;
			changed = 1;
		}
	}

	return changed;
}

static uint32_t ni_maschine_poll(struct ctlr_dev_t *base)
{
	struct ni_maschine_t *dev = (struct ni_maschine_t *)base;
	const int fd = dev->fd;
	uint8_t buf[128], src, *data;
	ssize_t nbytes;
	int i, pads_changed;
	int iter = 1;

	do {
		if ((nbytes = read(fd, &buf, sizeof(buf))) < 0) {
			perror("read");
			break;
		}

		src = buf[0];
		data = &buf[1];

		switch (src) {
		case 32:
			pads_changed = unpack_pads(dev, data, dev->pads);
			iter = 0;
			if (!pads_changed) {
				continue;
			}
			for (i = 0; i < 16; i++) {
				if(dev->pads[i] > 232)
					ni_maschine_pad_light_set(dev, i, 0x0077Fe,
							   PAD_PRESS_BRIGHTNESS);
				else
					ni_maschine_pad_light_set(dev, i, 0x101010,
							   PAD_RELEASE_BRIGHTNESS);
			}
			ni_maschine_led_flush(dev);
			break;
		case 1:
			button_dispatch(dev, data);
			iter = 0;
			break;
		default:
#ifdef NI_MASCHINE_DEBUG
			printf(" [?] %3ld bytes from %d\n", nbytes, src);
#endif
			iter = 0;
			break;
		}

		break;
	/* TODO: do just one iter */
	} while (iter);

	return 0;
}

static int32_t ni_maschine_disconnect(struct ctlr_dev_t *base)
{
#ifdef NI_MASCHINE_DEBUG
	printf("%s\n", __func__);
#endif
	struct ni_maschine_t *dev = (struct ni_maschine_t *)base;
	uint8_t *l = dev->light_buf;
	for(int i = 0; i <= 30; i++)
		l[i] = 0;
	for(int i = 0; i < 16; i++)
		ni_maschine_pad_light_set(dev, i, 0xF0F0F0, 0.f);
	ni_maschine_led_flush(dev);
	ni_maschine_set_brightness_contrast(dev, 0, 0);

	free(dev);
	return 0;
}

#endif /* OPENAV_CTLR_NI_MASCHINE */
