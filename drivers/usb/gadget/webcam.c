/*
 *	webcam.c -- USB webcam gadget driver
 *
 *	Copyright (C) 2009-2010
 *	    Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 *
 *	Modified for the Creality K1 camera (MJPEG + H.264)
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/usb/video.h>

#include "f_uvc.h"

/*
 * Kbuild is not very cooperative with respect to linking separately
 * compiled library objects into one module.  So for now we won't use
 * separate compilation ... ensuring init/exit sections work to shrink
 * the runtime footprint, and giving us at least some parts of what
 * a "gcc --combine ... part1.c part2.c part3.c ... " build would.
 */
#include "uvc_queue.c"
#include "uvc_video.c"
#include "uvc_v4l2.c"
#include "f_uvc.c"

USB_GADGET_COMPOSITE_OPTIONS();

/* --------------------------------------------------------------------------
 * Device descriptor
 */

#define WEBCAM_VENDOR_ID		0x1d6b	/* Linux Foundation */
/* Not upstream's 0x0102: that is ether.c's EEM gadget, and hosts name the
 * device from the USB ID database, so it showed up as "EEM Gadget". No
 * gadget uses 0x0110 and the database has no entry for it, so hosts fall
 * back to the product string below. */
#define WEBCAM_PRODUCT_ID		0x0110
#define WEBCAM_DEVICE_BCD		0x0010	/* 0.10 */

static char webcam_vendor_label[] = "thingino";
static char webcam_product_label[] = "K1 Camera";
static char webcam_config_label[] = "Video";
static char webcam_extension_label[] = "Thingino Camera Controls";

/* string IDs are assigned dynamically */

#define STRING_DESCRIPTION_IDX		USB_GADGET_FIRST_AVAIL_IDX
#define STRING_EXTENSION_IDX		(STRING_DESCRIPTION_IDX + 1)

static struct usb_string webcam_strings[] = {
	[USB_GADGET_MANUFACTURER_IDX].s = webcam_vendor_label,
	[USB_GADGET_PRODUCT_IDX].s = webcam_product_label,
	[USB_GADGET_SERIAL_IDX].s = "",
	[STRING_DESCRIPTION_IDX].s = webcam_config_label,
	[STRING_EXTENSION_IDX].s = webcam_extension_label,
	{  }
};

static struct usb_gadget_strings webcam_stringtab = {
	.language = 0x0409,
	.strings = webcam_strings,
};

static struct usb_gadget_strings *webcam_device_strings[] = {
	&webcam_stringtab,
	NULL,
};

static struct usb_device_descriptor webcam_device_descriptor = {
	.bLength		= USB_DT_DEVICE_SIZE,
	.bDescriptorType	= USB_DT_DEVICE,
	.bcdUSB			= cpu_to_le16(0x0200),
	.bDeviceClass		= USB_CLASS_MISC,
	.bDeviceSubClass	= 0x02,
	.bDeviceProtocol	= 0x01,
	.bMaxPacketSize0	= 0, /* dynamic */
	.idVendor		= cpu_to_le16(WEBCAM_VENDOR_ID),
	.idProduct		= cpu_to_le16(WEBCAM_PRODUCT_ID),
	.bcdDevice		= cpu_to_le16(WEBCAM_DEVICE_BCD),
	.iManufacturer		= 0, /* dynamic */
	.iProduct		= 0, /* dynamic */
	.iSerialNumber		= 0, /* dynamic */
	.bNumConfigurations	= 0, /* dynamic */
};

/* --------------------------------------------------------------------------
 * UVC control descriptors
 */

DECLARE_UVC_HEADER_DESCRIPTOR(1);

static const struct UVC_HEADER_DESCRIPTOR(1) uvc_control_header = {
	.bLength		= UVC_DT_HEADER_SIZE(1),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VC_HEADER,
	/* 1.1: the H.264 format below is frame-based, which UVC 1.1
	 * introduced. uvcd answers PROBE/COMMIT with the 34-byte 1.1
	 * streaming control. */
	.bcdUVC			= cpu_to_le16(0x0110),
	.wTotalLength		= 0, /* dynamic */
	.dwClockFrequency	= cpu_to_le32(48000000),
	.bInCollection		= 0, /* dynamic */
	.baInterfaceNr[0]	= 0, /* dynamic */
};

static const struct uvc_camera_terminal_descriptor uvc_camera_terminal = {
	.bLength		= UVC_DT_CAMERA_TERMINAL_SIZE(3),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VC_INPUT_TERMINAL,
	.bTerminalID		= 1,
	.wTerminalType		= cpu_to_le16(0x0201),
	.bAssocTerminal		= 0,
	.iTerminal		= 0,
	/* Digital zoom: the Zoom (Absolute) range uvcd answers with. */
	.wObjectiveFocalLengthMin	= cpu_to_le16(100),
	.wObjectiveFocalLengthMax	= cpu_to_le16(400),
	.wOcularFocalLength		= cpu_to_le16(100),
	.bControlSize		= 3,
	/* D1 auto-exposure mode, D2 auto-exposure priority, D3 exposure time
	 * (absolute), D9 zoom (absolute), D11 pan/tilt (absolute). */
	.bmControls[0]		= (1 << 1) | (1 << 2) | (1 << 3),
	.bmControls[1]		= (1 << 1) | (1 << 3),
	.bmControls[2]		= 0,
};

/*
 * UVC 1.1 appended bmVideoStandards to the Processing Unit descriptor, and
 * uvcvideo rejects a 1.1 device whose PU descriptor lacks it. The kernel
 * header only has the 1.0 layout.
 */
#define UVC_DT_PROCESSING_UNIT_1_1_SIZE(n)		(10 + (n))

struct uvc_processing_unit_descriptor_1_1 {
	__u8  bLength;
	__u8  bDescriptorType;
	__u8  bDescriptorSubType;
	__u8  bUnitID;
	__u8  bSourceID;
	__u16 wMaxMultiplier;
	__u8  bControlSize;
	__u8  bmControls[2];
	__u8  iProcessing;
	__u8  bmVideoStandards;
} __attribute__((packed));

static const struct uvc_processing_unit_descriptor_1_1 uvc_processing = {
	.bLength		= UVC_DT_PROCESSING_UNIT_1_1_SIZE(2),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VC_PROCESSING_UNIT,
	.bUnitID		= 2,
	.bSourceID		= 1,
	.wMaxMultiplier		= cpu_to_le16(16*1024),
	.bControlSize		= 2,
	/* D0 brightness, D1 contrast, D2 hue, D3 saturation, D4 sharpness,
	 * D5 gamma, D6 white balance temperature, D10 power line frequency,
	 * D12 white balance temperature auto. D8 backlight compensation is
	 * left out: at any strength the ISP's auto-exposure hunted. */
	.bmControls[0]		= (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6),
	.bmControls[1]		= (1 << 2) | (1 << 4),
	.iProcessing		= 0,
	.bmVideoStandards	= 0,	/* digital sensor, no analog standard */
};

DECLARE_UVC_EXTENSION_UNIT_DESCRIPTOR(1, 3);
static struct UVC_EXTENSION_UNIT_DESCRIPTOR(1, 3) uvc_extension = {
	.bLength = UVC_DT_EXTENSION_UNIT_SIZE(1, 3),
	.bDescriptorType = USB_DT_CS_INTERFACE,
	.bDescriptorSubType = UVC_VC_EXTENSION_UNIT,
	.bUnitID = 4,
	.guidExtensionCode = { 0x54, 0x48, 0x49, 0x4e, 0x47, 0x49, 0x4e, 0x4f,
		0x2d, 0x55, 0x56, 0x43, 0x44, 0x2d, 0x01, 0x00 },
	.bNumControls = 19,
	.bNrInPins = 1,
	.baSourceID = { 2 },
	.bControlSize = 3,
	/* Selectors 1-22: ISP (1-7, 9), factory reset (10), encoder settings
	 * (13-19), H.264 keyframe request (20), metering (21), rotation (22).
	 * Retired, their bits stay clear: 8 (defog, which had no effect),
	 * 11-12 (the old flips) and 23-25 (save, apply-on-boot and
	 * saved-query: uvcd persists every change itself again). */
	.bmControls = { 0x7f, 0xf3, 0x3f },
	.iExtension = 0,
};

static const struct uvc_output_terminal_descriptor uvc_output_terminal = {
	.bLength		= UVC_DT_OUTPUT_TERMINAL_SIZE,
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VC_OUTPUT_TERMINAL,
	.bTerminalID		= 3,
	.wTerminalType		= cpu_to_le16(0x0101),
	.bAssocTerminal		= 0,
	.bSourceID		= 2,
	.iTerminal		= 0,
};

/* --------------------------------------------------------------------------
 * UVC streaming descriptors — MJPEG (format 1) + H.264 framebased (format 2)
 */

DECLARE_UVC_INPUT_HEADER_DESCRIPTOR(1, 2);

static const struct UVC_INPUT_HEADER_DESCRIPTOR(1, 2) uvc_input_header = {
	.bLength		= UVC_DT_INPUT_HEADER_SIZE(1, 2),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VS_INPUT_HEADER,
	.bNumFormats		= 2,
	.wTotalLength		= 0, /* dynamic */
	.bEndpointAddress	= 0, /* dynamic */
	.bmInfo			= 0,
	.bTerminalLink		= 3,
	.bStillCaptureMethod	= 0,
	.bTriggerSupport	= 0,
	.bTriggerUsage		= 0,
	.bControlSize		= 1,
	.bmaControls[0][0]	= 0,
	.bmaControls[1][0]	= 0,
};

/* Every frame of both formats offers the same rates, fastest first, and
 * defaults to 30 fps. uvcd's uvcd_intervals[] must list the same ones. */
#define WEBCAM_FRAME_INTERVALS {				\
	cpu_to_le32(333333),	/* 30 fps */			\
	cpu_to_le32(400000),	/* 25 fps */			\
	cpu_to_le32(500000),	/* 20 fps */			\
	cpu_to_le32(666666),	/* 15 fps */			\
	cpu_to_le32(1000000),	/* 10 fps */			\
	cpu_to_le32(2000000),	/*  5 fps */			\
}

/* --- Format 1: MJPEG --- */

static const struct uvc_format_mjpeg uvc_format_mjpg = {
	.bLength		= UVC_DT_FORMAT_MJPEG_SIZE,
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VS_FORMAT_MJPEG,
	.bFormatIndex		= 1,
	.bNumFrameDescriptors	= 6,
	.bmFlags		= 0,
	.bDefaultFrameIndex	= 1,
	.bAspectRatioX		= 0,
	.bAspectRatioY		= 0,
	.bmInterfaceFlags	= 0,
	.bCopyProtect		= 0,
};

DECLARE_UVC_FRAME_MJPEG(6);

#define WEBCAM_FRAME_MJPEG(index, width, height, min_bps, max_bps) {	\
	.bLength		= UVC_DT_FRAME_MJPEG_SIZE(6),		\
	.bDescriptorType	= USB_DT_CS_INTERFACE,			\
	.bDescriptorSubType	= UVC_VS_FRAME_MJPEG,			\
	.bFrameIndex		= (index),				\
	.bmCapabilities		= 0,					\
	.wWidth			= cpu_to_le16(width),			\
	.wHeight		= cpu_to_le16(height),			\
	.dwMinBitRate		= cpu_to_le32(min_bps),			\
	.dwMaxBitRate		= cpu_to_le32(max_bps),			\
	.dwMaxVideoFrameBufferSize = cpu_to_le32((width) * (height) * 2),	\
	.dwDefaultFrameInterval	= cpu_to_le32(333333),			\
	.bFrameIntervalType	= 6,					\
	.dwFrameInterval	= WEBCAM_FRAME_INTERVALS,		\
}

static const struct UVC_FRAME_MJPEG(6) uvc_frame_mjpg_1080p =
	WEBCAM_FRAME_MJPEG(1, 1920, 1080, 3000000, 40000000);
static const struct UVC_FRAME_MJPEG(6) uvc_frame_mjpg_1280x960 =
	WEBCAM_FRAME_MJPEG(2, 1280, 960, 2000000, 30000000);
static const struct UVC_FRAME_MJPEG(6) uvc_frame_mjpg_720p =
	WEBCAM_FRAME_MJPEG(3, 1280, 720, 1500000, 25000000);
static const struct UVC_FRAME_MJPEG(6) uvc_frame_mjpg_800x600 =
	WEBCAM_FRAME_MJPEG(4, 800, 600, 1000000, 18000000);
static const struct UVC_FRAME_MJPEG(6) uvc_frame_mjpg_640x480 =
	WEBCAM_FRAME_MJPEG(5, 640, 480, 800000, 12000000);
static const struct UVC_FRAME_MJPEG(6) uvc_frame_mjpg_360p =
	WEBCAM_FRAME_MJPEG(6, 640, 360, 1000000, 10000000);

/* --- Format 2: H.264 (Frame-Based) --- */

/*
 * UVC 1.1 frame-based format/frame descriptors for H.264.
 * The kernel headers define UVC_VS_FORMAT_FRAME_BASED (0x10) and
 * UVC_VS_FRAME_FRAME_BASED (0x11) but lack the corresponding structs.
 */

#define UVC_DT_FORMAT_FRAMEBASED_SIZE			28
#define UVC_DT_FRAME_FRAMEBASED_SIZE(n)			(26 + 4 * (n))

struct uvc_format_framebased {
	__u8  bLength;
	__u8  bDescriptorType;
	__u8  bDescriptorSubType;
	__u8  bFormatIndex;
	__u8  bNumFrameDescriptors;
	__u8  guidFormat[16];
	__u8  bBitsPerPixel;
	__u8  bDefaultFrameIndex;
	__u8  bAspectRatioX;
	__u8  bAspectRatioY;
	__u8  bmInterfaceFlags;
	__u8  bCopyProtect;
	__u8  bVariableSize;
} __attribute__((packed));

#define DECLARE_UVC_FRAME_FRAMEBASED(n)				\
	struct UVC_FRAME_FRAMEBASED_##n {			\
		__u8  bLength;					\
		__u8  bDescriptorType;				\
		__u8  bDescriptorSubType;			\
		__u8  bFrameIndex;				\
		__u8  bmCapabilities;				\
		__u16 wWidth;					\
		__u16 wHeight;					\
		__u32 dwMinBitRate;				\
		__u32 dwMaxBitRate;				\
		__u32 dwDefaultFrameInterval;			\
		__u8  bFrameIntervalType;			\
		__u32 dwBytesPerLine;				\
		__u32 dwFrameInterval[];			\
	} __attribute__((packed))

DECLARE_UVC_FRAME_FRAMEBASED(6);

static const struct uvc_format_framebased uvc_format_h264 = {
	.bLength		= UVC_DT_FORMAT_FRAMEBASED_SIZE,
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VS_FORMAT_FRAME_BASED,
	.bFormatIndex		= 2,
	.bNumFrameDescriptors	= 6,
	.guidFormat		=
		{ 'H',  '2',  '6',  '4', 0x00, 0x00, 0x10, 0x00,
		 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71},
	.bBitsPerPixel		= 0,
	.bDefaultFrameIndex	= 1,
	.bAspectRatioX		= 0,
	.bAspectRatioY		= 0,
	.bmInterfaceFlags	= 0,
	.bCopyProtect		= 0,
	.bVariableSize		= 1,
};

#define WEBCAM_FRAME_H264(index, width, height, min_bps, max_bps) {	\
	.bLength		= UVC_DT_FRAME_FRAMEBASED_SIZE(6),	\
	.bDescriptorType	= USB_DT_CS_INTERFACE,			\
	.bDescriptorSubType	= UVC_VS_FRAME_FRAME_BASED,		\
	.bFrameIndex		= (index),				\
	.bmCapabilities		= 0,					\
	.wWidth			= cpu_to_le16(width),			\
	.wHeight		= cpu_to_le16(height),			\
	.dwMinBitRate		= cpu_to_le32(min_bps),			\
	.dwMaxBitRate		= cpu_to_le32(max_bps),			\
	.dwDefaultFrameInterval	= cpu_to_le32(333333),			\
	.bFrameIntervalType	= 6,					\
	.dwBytesPerLine		= 0,					\
	.dwFrameInterval	= WEBCAM_FRAME_INTERVALS,		\
}

static const struct UVC_FRAME_FRAMEBASED_6 uvc_frame_h264_1080p =
	WEBCAM_FRAME_H264(1, 1920, 1080, 500000, 8000000);
static const struct UVC_FRAME_FRAMEBASED_6 uvc_frame_h264_1280x960 =
	WEBCAM_FRAME_H264(2, 1280, 960, 400000, 6000000);
static const struct UVC_FRAME_FRAMEBASED_6 uvc_frame_h264_720p =
	WEBCAM_FRAME_H264(3, 1280, 720, 300000, 5000000);
static const struct UVC_FRAME_FRAMEBASED_6 uvc_frame_h264_800x600 =
	WEBCAM_FRAME_H264(4, 800, 600, 200000, 3500000);
static const struct UVC_FRAME_FRAMEBASED_6 uvc_frame_h264_640x480 =
	WEBCAM_FRAME_H264(5, 640, 480, 150000, 3000000);
static const struct UVC_FRAME_FRAMEBASED_6 uvc_frame_h264_360p =
	WEBCAM_FRAME_H264(6, 640, 360, 100000, 2000000);

/* --- Color matching --- */

static const struct uvc_color_matching_descriptor uvc_color_matching = {
	.bLength		= UVC_DT_COLOR_MATCHING_SIZE,
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= UVC_VS_COLORFORMAT,
	.bColorPrimaries	= 1,
	.bTransferCharacteristics	= 1,
	.bMatrixCoefficients	= 4,
};

/* --------------------------------------------------------------------------
 * Descriptor arrays
 *
 * None of the class-specific descriptors depend on bus speed -- f_uvc
 * supplies the per-speed endpoint descriptors itself -- so one list of
 * each kind serves every speed.
 */

static const struct uvc_descriptor_header * const uvc_control_cls[] = {
	(const struct uvc_descriptor_header *) &uvc_control_header,
	(const struct uvc_descriptor_header *) &uvc_camera_terminal,
	(const struct uvc_descriptor_header *) &uvc_processing,
	(const struct uvc_descriptor_header *) &uvc_extension,
	(const struct uvc_descriptor_header *) &uvc_output_terminal,
	NULL,
};

static const struct uvc_descriptor_header * const uvc_streaming_cls[] = {
	(const struct uvc_descriptor_header *) &uvc_input_header,
	(const struct uvc_descriptor_header *) &uvc_format_mjpg,
	(const struct uvc_descriptor_header *) &uvc_frame_mjpg_1080p,
	(const struct uvc_descriptor_header *) &uvc_frame_mjpg_1280x960,
	(const struct uvc_descriptor_header *) &uvc_frame_mjpg_720p,
	(const struct uvc_descriptor_header *) &uvc_frame_mjpg_800x600,
	(const struct uvc_descriptor_header *) &uvc_frame_mjpg_640x480,
	(const struct uvc_descriptor_header *) &uvc_frame_mjpg_360p,
	(const struct uvc_descriptor_header *) &uvc_format_h264,
	(const struct uvc_descriptor_header *) &uvc_frame_h264_1080p,
	(const struct uvc_descriptor_header *) &uvc_frame_h264_1280x960,
	(const struct uvc_descriptor_header *) &uvc_frame_h264_720p,
	(const struct uvc_descriptor_header *) &uvc_frame_h264_800x600,
	(const struct uvc_descriptor_header *) &uvc_frame_h264_640x480,
	(const struct uvc_descriptor_header *) &uvc_frame_h264_360p,
	(const struct uvc_descriptor_header *) &uvc_color_matching,
	NULL,
};

/* --------------------------------------------------------------------------
 * USB configuration
 */

static struct usb_function_instance *fi_acm;
static struct usb_function *f_acm;

/* Names for the ACM console in place of f_acm's generic "CDC ..." ones.
 * The order is f_acm's ACM_CTRL_IDX, ACM_DATA_IDX, ACM_IAD_IDX. */
static struct usb_string webcam_acm_string_defs[] = {
	{ .s = "K1 Camera Serial Console" },
	{ .s = "K1 Camera Serial Data" },
	{ .s = "K1 Camera Serial Console" },
	{  }
};

static struct usb_gadget_strings webcam_acm_string_table = {
	.language = 0x0409,
	.strings = webcam_acm_string_defs,
};

static struct usb_gadget_strings *webcam_acm_strings[] = {
	&webcam_acm_string_table,
	NULL,
};

static int __init
webcam_config_bind(struct usb_configuration *c)
{
	int ret;

	ret = uvc_bind_config(c, uvc_control_cls, uvc_control_cls,
		uvc_streaming_cls, uvc_streaming_cls, uvc_streaming_cls);
	if (ret < 0)
		return ret;

	if (fi_acm) {
		f_acm = usb_get_function(fi_acm);
		if (IS_ERR(f_acm)) {
			pr_warn("g_webcam: ACM get_function failed: %ld\n",
				PTR_ERR(f_acm));
			f_acm = NULL;
		} else {
			f_acm->strings = webcam_acm_strings;
			ret = usb_add_function(c, f_acm);
			if (ret < 0)
				pr_warn("g_webcam: ACM add_function failed: %d\n", ret);
		}
	}

	return 0;
}

static struct usb_configuration webcam_config_driver = {
	.label			= webcam_config_label,
	.bConfigurationValue	= 1,
	.iConfiguration		= 0, /* dynamic */
	/* The K1 camera runs entirely off VBUS, so claim bus power and the
	 * full USB 2.0 budget rather than the Kconfig default of 2 mA. */
	.bmAttributes		= 0,
	.MaxPower		= 500,
};

static int /* __init_or_exit */
webcam_unbind(struct usb_composite_dev *cdev)
{
	if (f_acm)
		usb_put_function(f_acm);
	if (fi_acm)
		usb_put_function_instance(fi_acm);
	return 0;
}

static int __init
webcam_bind(struct usb_composite_dev *cdev)
{
	int ret;

	ret = usb_string_ids_tab(cdev, webcam_strings);
	if (ret < 0)
		goto error;
	webcam_device_descriptor.iManufacturer =
		webcam_strings[USB_GADGET_MANUFACTURER_IDX].id;
	webcam_device_descriptor.iProduct =
		webcam_strings[USB_GADGET_PRODUCT_IDX].id;
	webcam_config_driver.iConfiguration =
		webcam_strings[STRING_DESCRIPTION_IDX].id;
	uvc_extension.iExtension = webcam_strings[STRING_EXTENSION_IDX].id;

	/* ACM serial: get function instance from f_acm module */
	fi_acm = usb_get_function_instance("acm");
	if (IS_ERR(fi_acm)) {
		pr_warn("g_webcam: ACM function not available: %ld\n",
			PTR_ERR(fi_acm));
		fi_acm = NULL;
	}

	if ((ret = usb_add_config(cdev, &webcam_config_driver,
					webcam_config_bind)) < 0)
		goto error;

	usb_composite_overwrite_options(cdev, &coverwrite);
	INFO(cdev, "K1 Camera Gadget\n");
	return 0;

error:
	webcam_unbind(cdev);
	return ret;
}

/* --------------------------------------------------------------------------
 * Driver
 */

static __refdata struct usb_composite_driver webcam_driver = {
	.name		= "g_webcam",
	.dev		= &webcam_device_descriptor,
	.strings	= webcam_device_strings,
	.max_speed	= USB_SPEED_SUPER,
	.bind		= webcam_bind,
	.unbind		= webcam_unbind,
};

static int __init
webcam_init(void)
{
	return usb_composite_probe(&webcam_driver);
}

static void __exit
webcam_cleanup(void)
{
	usb_composite_unregister(&webcam_driver);
}

module_init(webcam_init);
module_exit(webcam_cleanup);

MODULE_AUTHOR("Laurent Pinchart");
MODULE_DESCRIPTION("K1 Camera Gadget");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.2.0");
