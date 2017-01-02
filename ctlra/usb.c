#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ctlra.h"
#include "devices.h"
#include "device_impl.h"

#include "libusb.h"

#define USB_PATH_MAX 256

static int ctlra_libusb_initialized;
static struct libusb_context *ctx = 0;

static int ctlra_usb_impl_get_serial(struct libusb_device_handle *handle,
				     uint8_t desc_serial, uint8_t *buffer,
				     uint32_t buf_size)
{
	if (desc_serial > 0) {
		int ret = libusb_get_string_descriptor_ascii(handle,
							     desc_serial,
							     buffer,
							     buf_size);
		if (ret < 0)
			return -1;
	}
	return 0;
}

static void hotplug_func(struct ctlra_dev_t* dev, uint32_t num_events,
		  struct ctlra_event_t** event, void *userdata)
{
	printf("%s\n", __func__);
}

static int ctlra_usb_impl_hotplug_cb(libusb_context *ctx,
                                     libusb_device *dev,
                                     libusb_hotplug_event event,
                                     void *user_data)
{
	int ret;
	struct libusb_device_descriptor desc;
	ret = libusb_get_device_descriptor(dev, &desc);
	if(ret != LIBUSB_SUCCESS) {
		printf("Error getting device descriptor\n");
		return -1;
	}
	if(event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {

		libusb_device_handle *handle = 0;
		ret = libusb_open(dev, &handle);
		if (ret != LIBUSB_SUCCESS)
			return -1;
		uint8_t buf[255];
		ctlra_usb_impl_get_serial(handle, desc.iSerialNumber,
					  buf, 255);

		printf("Device attached: %04x:%04x, serial %s, ctlra %p\n",
		       desc.idVendor, desc.idProduct, buf, user_data);

		// call application "hotplug accept" callback here,
		// which provides the event func / ud pair
		
		printf("calling connect now\n");
		ctlra_dev_connect((struct ctlra_t *)user_data,
				  CTLRA_DEV_NI_KONTROL_X1_MK2,
				  hotplug_func, 0x0, 0x0);
	}
	if(event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
		printf("Device removed: %04x:%04x, serial %d\n",
		       desc.idVendor, desc.idProduct, desc.iSerialNumber);

	printf("%s: done & return 0\n", __func__);
	return 0;
}


void ctlra_impl_usb_idle_iter(struct ctlra_t *ctlra)
{
	struct timeval tv = {0};
	/* 1st: NULL context
	 * 2nd: timeval to wait - 0 returns as if non blocking
	 * 3rd: int* to completed event - unused by Ctlra */
	libusb_handle_events_timeout_completed(NULL, &tv, NULL);
}


int ctlra_dev_impl_usb_init(struct ctlra_t *ctlra)
{
	int ret;
	/* TODO: move this to a usb specific cltra_init() function */
	if(ctlra_libusb_initialized)
		return -1;

	ret = libusb_init (&ctx);
	if (ret < 0) {
		printf("failed to initialise libusb: %s\n",
		       libusb_error_name(ret));
		return -1;
	}
	ctlra_libusb_initialized = 1;

	if(!libusb_has_capability (LIBUSB_CAP_HAS_HOTPLUG)) {
		printf ("Ctlra: No Hotplug on this platform\n");
		return -2;
	}

	/* setup hotplug callbacks */
	libusb_hotplug_callback_handle hp[2];
	ret = libusb_hotplug_register_callback(NULL,
					       LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED,
					       /* TODO: do we want to gracefully
						* remove? We should handle
						* unexpected unplug anyway.. */
					       /* | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, */
					       0,
					       LIBUSB_HOTPLUG_MATCH_ANY,
					       LIBUSB_HOTPLUG_MATCH_ANY,
					       LIBUSB_HOTPLUG_MATCH_ANY,
					       ctlra_usb_impl_hotplug_cb,
					       ctlra,
					       &hp[0]);
	if (ret != LIBUSB_SUCCESS) {
		printf("hotplug register failure\n");
	}
	return 0;
}

int ctlra_dev_impl_usb_open(struct ctlra_dev_t *ctlra_dev, int vid,
                            int pid)
{
	int ret;

	libusb_device **devs;
	libusb_device *dev;
	int i = 0, j = 0;
	uint8_t path[USB_PATH_MAX];

	int cnt = libusb_get_device_list(NULL, &devs);
	if (cnt < 0)
		goto fail;

	while ((dev = devs[i++]) != NULL) {
		struct libusb_device_descriptor desc;
		int r = libusb_get_device_descriptor(dev, &desc);
		if (r < 0) {
			printf("failed to get device descriptor");
			goto fail;
		}
#if 0
		printf("%04x:%04x (serial %d) (bus %d, device %d)",
		       desc.idVendor, desc.idProduct, desc.iSerialNumber,
		       libusb_get_bus_number(dev),
		       libusb_get_device_address(dev));

		r = libusb_get_port_numbers(dev, path, sizeof(path));
		if (r > 0) {
			printf(" path: %d", path[0]);
			for (j = 1; j < r; j++)
				printf(".%d", path[j]);
		}
		printf("\n");
#endif

		if(desc.idVendor  == vid &&
		    desc.idProduct == pid) {
			ctlra_dev->info.serial_number = desc.iSerialNumber;
			ctlra_dev->info.vendor_id     = desc.idVendor;
			ctlra_dev->info.device_id     = desc.idProduct;
			break;
		}
	}

	libusb_free_device_list(devs, 1);

	if(!dev)
		goto fail;
	ctlra_dev->usb_device = dev;

	memset(ctlra_dev->usb_interface, 0,
	       sizeof(ctlra_dev->usb_interface));

	return 0;
fail:
	return -1;
}

int ctlra_dev_impl_usb_open_interface(struct ctlra_dev_t *ctlra_dev,
                                      int interface,
                                      int handle_idx)
{
	if(handle_idx >= CTLRA_USB_IFACE_PER_DEV) {
		printf("request for handle beyond available iface per dev range\n");
		return -1;
	}
	libusb_device *usb_dev = ctlra_dev->usb_device;
	libusb_device_handle *handle = 0;

	/* now that we've found the device, open the handle */
	int ret = libusb_open(usb_dev, &handle);
	if(ret != LIBUSB_SUCCESS) {
		printf("Error in claiming interface\n");
		return -1;
	}

	ctlra_usb_impl_get_serial(handle, ctlra_dev->info.serial_number,
				  (uint8_t*)ctlra_dev->info.serial,
				  CTLRA_DEV_SERIAL_MAX);

	/* enable auto management of kernel claiming / unclaiming */
	if (libusb_has_capability(LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER)) {
		ret = libusb_set_auto_detach_kernel_driver(handle, 1);
		if(ret != LIBUSB_SUCCESS) {
			printf("Error setting auto kernel unclaiming\n");
			return -1;
		}
	} else {
		printf("Warning: auto kernel claim/unclaiming not supported\n");
	}

	ret = libusb_claim_interface(handle, interface);
	if(ret != LIBUSB_SUCCESS) {
		printf("Error in claiming interface\n");
		int kernel_active = libusb_kernel_driver_active(handle,
		                    interface);
		if(kernel_active)
			printf("=> Kernel has claimed the interface. Stop"
			       "other applications using this device and retry\n");
		return -1;
	}

	/* Commit to success: update handles in struct and return ok*/
	ctlra_dev->usb_interface[handle_idx] = handle;

	return 0;
}

int ctlra_dev_impl_usb_interrupt_read(struct ctlra_dev_t *dev, uint32_t idx,
                                      uint32_t endpoint, uint8_t *data,
                                      uint32_t size)
{
	int transferred;
	const uint32_t timeout = 10;
	int r = libusb_interrupt_transfer(dev->usb_interface[idx], endpoint,
	                                  data, size, &transferred, timeout);
	if(r == LIBUSB_ERROR_TIMEOUT)
		return 0;
	if (r < 0) {
		fprintf(stderr, "intr error %d\n", r);
		return r;
	}
	return transferred;
}

int ctlra_dev_impl_usb_interrupt_write(struct ctlra_dev_t *dev, uint32_t idx,
                                       uint32_t endpoint, uint8_t *data,
                                       uint32_t size)
{
	int transferred;
	const uint32_t timeout = 100;
	int r = libusb_interrupt_transfer(dev->usb_interface[idx], endpoint,
	                                  data, size, &transferred, timeout);
	if (r < 0) {
		fprintf(stderr, "intr error %d\n", r);
		return r;
	}
	return transferred;
}

int ctlra_dev_impl_usb_bulk_write(struct ctlra_dev_t *dev, uint32_t idx,
                                  uint32_t endpoint, uint8_t *data,
                                  uint32_t size)
{
	const uint32_t timeout = 1000;
	int transferred;
	int ret = libusb_bulk_transfer(dev->usb_interface[idx], endpoint,
	                               data, size, &transferred, timeout);
	if (ret < 0) {
		fprintf(stderr, "%s intr error %d\n", __func__, ret);
		return ret;
	}
	return transferred;

}

void ctlra_dev_impl_usb_close(struct ctlra_dev_t *dev)
{
	for(int i = 0; i < CTLRA_USB_IFACE_PER_DEV; i++) {
		if(dev->usb_interface[i]) {
#if 0
			// Running this always seems to throw an error,
			// and it has no negative side-effects to not?
			int ret = libusb_release_interface(dev->usb_device, i);
			if(ret == LIBUSB_ERROR_NOT_FOUND) {
				// Seems to always happen? LibUSB bug?
				//printf("%s: release interface error: interface %d not found, continuing...\n", __func__, i);
			} else if (ret == LIBUSB_ERROR_NO_DEVICE)
				printf("%s: release interface error: no device, continuing...\n", __func__);
			else if(ret < 0) {
				printf("%s:Ctrla Warning: release interface ret: %d\n", __func__, ret);
			}
#endif
			libusb_close(dev->usb_interface[i]);
		}
	}
}

void ctlra_impl_usb_shutdown()
{
	libusb_exit(ctx);
}

