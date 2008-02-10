/*
 * AuthenTec AES4000 driver for libfprint
 * Copyright (C) 2007-2008 Daniel Drake <dsd@gentoo.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "aes4000"

#include <errno.h>

#include <glib.h>
#include <libusb.h>

#include <aeslib.h>
#include <fp_internal.h>

#define CTRL_TIMEOUT	1000
#define EP_IN			(1 | LIBUSB_ENDPOINT_IN)
#define EP_OUT			(2 | LIBUSB_ENDPOINT_OUT)
#define DATA_BUFLEN		0x1259
#define NR_SUBARRAYS	6
#define SUBARRAY_LEN	768

struct aes4k_dev {
	libusb_urb_handle *img_trf;
};

static const struct aes_regwrite init_reqs[] = {
	/* master reset */
	{ 0x80, 0x01 },
	{ 0, 0 },
	{ 0x80, 0x00 },
	{ 0, 0 },

	{ 0x81, 0x00 },
	{ 0x80, 0x00 },
	{ 0, 0 },

	/* scan reset */
	{ 0x80, 0x02 },
	{ 0, 0 },
	{ 0x80, 0x00 },
	{ 0, 0 },

	/* disable register buffering */
	{ 0x80, 0x04 },
	{ 0, 0 },
	{ 0x80, 0x00 },
	{ 0, 0 },

	{ 0x81, 0x00 },
	{ 0, 0 },
	/* windows driver reads registers now (81 02) */
	{ 0x80, 0x00 },
	{ 0x81, 0x00 },

	/* set excitation bias current: 2mhz drive ring frequency,
	 * 4V drive ring voltage, 16.5mA excitation bias */
	{ 0x82, 0x04 },

	/* continuously sample drive ring for finger detection,
	 * 62.50ms debounce delay */
	{ 0x83, 0x13 },

	{ 0x84, 0x07 }, /* set calibration resistance to 12 kiloohms */
	{ 0x85, 0x3d }, /* set calibration capacitance */
	{ 0x86, 0x03 }, /* detect drive voltage */
	{ 0x87, 0x01 }, /* set detection frequency to 125khz */
	{ 0x88, 0x02 }, /* set column scan period */
	{ 0x89, 0x02 }, /* set measure drive */
	{ 0x8a, 0x33 }, /* set measure frequency and sense amplifier bias */
	{ 0x8b, 0x33 }, /* set matrix pattern */
	{ 0x8c, 0x0f }, /* set demodulation phase 1 */
	{ 0x8d, 0x04 }, /* set demodulation phase 2 */
	{ 0x8e, 0x23 }, /* set sensor gain */
	{ 0x8f, 0x07 }, /* set image parameters */
	{ 0x90, 0x00 }, /* carrier offset null */
	{ 0x91, 0x1c }, /* set A/D reference high */
	{ 0x92, 0x08 }, /* set A/D reference low */
	{ 0x93, 0x00 }, /* set start row to 0 */
	{ 0x94, 0x05 }, /* set end row to 5 */
	{ 0x95, 0x00 }, /* set start column to 0 */
	{ 0x96, 0x18 }, /* set end column to 24*4=96 */
	{ 0x97, 0x04 }, /* data format and thresholds */
	{ 0x98, 0x28 }, /* image data control */
	{ 0x99, 0x00 }, /* disable general purpose outputs */
	{ 0x9a, 0x0b }, /* set initial scan state */
	{ 0x9b, 0x00 }, /* clear challenge word bits */
	{ 0x9c, 0x00 }, /* clear challenge word bits */
	{ 0x9d, 0x09 }, /* set some challenge word bits */
	{ 0x9e, 0x53 }, /* clear challenge word bits */
	{ 0x9f, 0x6b }, /* set some challenge word bits */
	{ 0, 0 },

	{ 0x80, 0x00 },
	{ 0x81, 0x00 },
	{ 0, 0 },
	{ 0x81, 0x04 },
	{ 0, 0 },
	{ 0x81, 0x00 },
};

static void do_capture(struct fp_img_dev *dev);

static void img_cb(libusb_dev_handle *devh, libusb_urb_handle *urbh,
	enum libusb_urb_cb_status status, unsigned char endpoint,
	int rqlength, unsigned char *data, int actual_length, void *user_data)
{
	struct fp_img_dev *dev = user_data;
	struct aes4k_dev *aesdev = dev->priv;
	unsigned char *ptr = data;
	struct fp_img *img;
	int i;

	if (status == FP_URB_CANCELLED) {
		goto err;
	} else if (status != FP_URB_COMPLETED) {
		fpi_imgdev_session_error(dev, -EIO);
		goto err;
	} else if (rqlength != actual_length) {
		fpi_imgdev_session_error(dev, -EPROTO);
		goto err;
	}

	fpi_imgdev_report_finger_status(dev, TRUE);

	img = fpi_img_new_for_imgdev(dev);
	img->flags = FP_IMG_COLORS_INVERTED | FP_IMG_V_FLIPPED | FP_IMG_H_FLIPPED;
	for (i = 0; i < NR_SUBARRAYS; i++) {
		fp_dbg("subarray header byte %02x", *ptr);
		ptr++;
		aes_assemble_image(ptr, 96, 16, img->data + (i * 96 * 16));
		ptr += SUBARRAY_LEN;
	}

	fpi_imgdev_image_captured(dev, img);

	/* FIXME: rather than assuming finger has gone, we should poll regs until
	 * it really has, then restart the capture */
	fpi_imgdev_report_finger_status(dev, FALSE);

	do_capture(dev);

err:
	g_free(data);
	aesdev->img_trf = NULL;
	libusb_urb_handle_free(urbh);
}

static void do_capture(struct fp_img_dev *dev)
{
	struct aes4k_dev *aesdev = dev->priv;
	struct libusb_bulk_transfer trf = {
		.endpoint = EP_IN,
		.length = DATA_BUFLEN,
		.data = g_malloc(DATA_BUFLEN),
	};

	aesdev->img_trf = libusb_async_bulk_transfer(dev->udev, &trf, img_cb, dev,
		0);
	if (!aesdev->img_trf) {
		g_free(trf.data);
		fpi_imgdev_session_error(dev, -EIO);
	}
}

static void init_reqs_cb(struct fp_img_dev *dev, int result)
{
	fpi_imgdev_activate_complete(dev, result);
	if (result == 0)
		do_capture(dev);
}

static int dev_activate(struct fp_img_dev *dev, enum fp_imgdev_state state)
{
	aes_write_regv(dev, init_reqs, G_N_ELEMENTS(init_reqs), init_reqs_cb);
	return 0;
}

static void dev_deactivate(struct fp_img_dev *dev)
{
	struct aes4k_dev *aesdev = dev->priv;

	/* FIXME: should wait for cancellation to complete before returning
	 * from deactivation, otherwise app may legally exit before we've
	 * cleaned up */
	if (aesdev->img_trf)
		libusb_urb_handle_cancel(dev->udev, aesdev->img_trf);
	fpi_imgdev_deactivate_complete(dev);
}

static int dev_init(struct fp_img_dev *dev, unsigned long driver_data)
{
	int r;

	r = libusb_claim_interface(dev->udev, 0);
	if (r < 0)
		fp_err("could not claim interface 0");

	dev->priv = g_malloc0(sizeof(struct aes4k_dev));

	if (r == 0)
		fpi_imgdev_init_complete(dev, 0);

	return r;
}

static void dev_deinit(struct fp_img_dev *dev)
{
	g_free(dev->priv);
	libusb_release_interface(dev->udev, 0);
	fpi_imgdev_deinit_complete(dev);
}

static const struct usb_id id_table[] = {
	{ .vendor = 0x08ff, .product = 0x5501 },
	{ 0, 0, 0, },
};

struct fp_img_driver aes4000_driver = {
	.driver = {
		.id = 3,
		.name = FP_COMPONENT,
		.full_name = "AuthenTec AES4000",
		.id_table = id_table,
	},
	.flags = 0,
	.img_height = 96,
	.img_width = 96,
	.enlarge_factor = 3,

	/* temporarily lowered until image quality improves */
	.bz3_threshold = 9,

	.init = dev_init,
	.deinit = dev_deinit,
	.activate = dev_activate,
	.deactivate = dev_deactivate,
};

