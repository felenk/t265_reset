// Copyright(c) 2020 Intel Corporation. All Rights Reserved.

#include <stdio.h>
#include <iostream>
#include <libusb.h>
#include "log.h"
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>

using namespace std;

#define MOVIDIUS_VID 0x03e7
#define MOVIDIUS_PID 0x2150

#define USB_PORT_FEAT_POWER      (1 << 3)
#define USB_CTRL_GET_TIMEOUT     5000
#define USB_SS_BCD               0x0300

#if defined(__cplusplus) && defined(_MSC_VER)
    // libusb binary release on Windows is compiled against old VS2015?
    // DO NOT add this to librealsense. It's only used for testing this single binary on Windows.
    FILE iob[] = { *stdin, *stdout, *stderr };
    extern "C" {
        FILE* __cdecl _iob(void) { return iob; }
    }
#endif


#pragma pack(push,1)
struct usb_hub_descriptor {
    unsigned char bDescLength;
    unsigned char bDescriptorType;
    unsigned char bNbrPorts;
    unsigned char wHubCharacteristics[2];
    unsigned char bPwrOn2PwrGood;
    unsigned char bHubContrCurrent;
    unsigned char data[1]; /* use 1 to avoid zero-sized array warning */
};
#pragma pack(pop)

struct hub_info {
    struct libusb_device *dev;
    int portCount;
    string containerId;
    string vendor;
    int level;
};


int getHubInfo(struct libusb_device* dev, struct hub_info *info) {
    int rc;
    int len = 0;
    unsigned char buf[LIBUSB_DT_HUB_NONVAR_SIZE + 2 + 3] = {0};
    struct libusb_device_handle *devh = nullptr;
    int minlen = LIBUSB_DT_HUB_NONVAR_SIZE + 2;

    struct libusb_device_descriptor desc;
    rc = libusb_get_device_descriptor(dev, &desc);
    if (rc) {
        return rc;
    }
    if (desc.bDeviceClass != LIBUSB_CLASS_HUB) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }

    int bcd_usb = libusb_le16_to_cpu(desc.bcdUSB);
    int desc_type = bcd_usb >= USB_SS_BCD ? LIBUSB_DT_SUPERSPEED_HUB
                                         : LIBUSB_DT_HUB;

    rc = libusb_open(dev, &devh);
    if (rc == LIBUSB_SUCCESS) {
       struct usb_hub_descriptor *uhd = (struct usb_hub_descriptor *)buf;

       len = libusb_control_transfer(devh,
           LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS
                              | LIBUSB_RECIPIENT_DEVICE, /* hub status */
           LIBUSB_REQUEST_GET_DESCRIPTOR,
           desc_type << 8,
           0,
           buf, sizeof(buf),
           USB_CTRL_GET_TIMEOUT
       );

        if (len >= minlen) {


           /* Get container_id: */
           struct libusb_bos_descriptor *bos;
           rc = libusb_get_bos_descriptor(devh, &bos);
           if (rc < 0) {
               rc = libusb_get_bos_descriptor(devh, &bos);
           }

           if (rc == LIBUSB_SUCCESS) {
               int cap;
               char cid[33];
               std::memset(cid, 0, sizeof(cid));
               for (cap=0; cap < bos->bNumDeviceCaps; cap++) {
                   if (bos->dev_capability[cap]->bDevCapabilityType == LIBUSB_BT_CONTAINER_ID) {
                       struct libusb_container_id_descriptor *container_id;
                       rc = libusb_get_container_id_descriptor(nullptr, bos->dev_capability[cap], &container_id);
                       if (rc == 0) {
                           int i;
                           for (i=0; i<16; i++) {
                               sprintf(cid+i*2, "%02x", container_id->ContainerID[i]);
                           }
                           cid[i*2] = 0;
                           libusb_free_container_id_descriptor(container_id);
                       }
                   }
               }

               libusb_free_bos_descriptor(bos);

               int ports = libusb_get_port_numbers(dev, buf, 8);
               int level = ports + 1;
               char vendor[16];
               snprintf(vendor, sizeof(vendor), "%04x:%04x",
                     libusb_le16_to_cpu(desc.idVendor),
                     libusb_le16_to_cpu(desc.idProduct)
               );



               /* Raspberry Pi 4 hack for USB3 root hub - from uhubctl */
               if (strlen(cid) == 0 &&
                   strcasecmp(vendor, "1d6b:0003") == 0 &&
                   level == 1 &&
                   uhd->bNbrPorts == 4 &&
                   bcd_usb == USB_SS_BCD)
               {

                   strcpy(cid, "5cf3ee30d5074925b001802d79434c30");
               }

               info->dev = dev;
               info->vendor = string(vendor);
               info->containerId = string(cid);
               info->portCount = uhd->bNbrPorts;

           } else {
               LOGE("Error %d while trying to obtain bos descriptor.", rc);
           }
       } else {
           rc = len;
       }

       if (devh) {
           libusb_close(devh);
       }
    }

    return rc;
}


int getVirtualHubs(libusb_context *context, libusb_device *hub, std::vector<libusb_device*> &virtualHubs) {
    libusb_device **deviceList = nullptr;
    ssize_t deviceCount = 0;
    unsigned char buf[8];

    deviceCount = libusb_get_device_list(context, &deviceList);

    libusb_device_descriptor hubDesc = {0};
    int rc = libusb_get_device_descriptor(hub, &hubDesc);
    if (rc != LIBUSB_SUCCESS) {
        // At least attempt to recycle one hub (parent).
        virtualHubs.push_back(hub);

        libusb_free_device_list(deviceList, 1);
        return rc;
    }

    struct hub_info directParentHubInfo;
    rc = getHubInfo(hub, &directParentHubInfo);

    if (rc != LIBUSB_SUCCESS) {
        LOGE("Unable to get container ID for hub. Will not be able to power-cycle all virtual hubs.");
    }

    for (size_t hIdx = 0; hIdx < deviceCount; ++hIdx) {
        libusb_device *usbDevice = deviceList[hIdx];
        libusb_device_descriptor usbDeviceDesc = {0};
        int rc = libusb_get_device_descriptor(usbDevice, &usbDeviceDesc);

        if (rc == LIBUSB_SUCCESS) {
            if (usbDeviceDesc.bDeviceClass == LIBUSB_CLASS_HUB) {
                if (usbDeviceDesc.idVendor != hubDesc.idVendor) {
                    continue;
                }

                struct hub_info deviceInfo;
                rc = getHubInfo(usbDevice, &deviceInfo);
                if (rc != LIBUSB_SUCCESS) {
                    LOGE("Unable to obtain hub info for %04x:%04x", usbDeviceDesc.idVendor, usbDeviceDesc.idProduct);
                    continue;
                }

                if (directParentHubInfo.containerId != deviceInfo.containerId ||
                    directParentHubInfo.portCount != deviceInfo.portCount) {
                    continue;
                }

                LOGD("Will power cycle virtual hub %04x:%04x", usbDeviceDesc.idVendor, usbDeviceDesc.idProduct);
                virtualHubs.push_back(usbDevice);

            }
        } else {
            LOGD("Failed to query device descriptor on hub device");
        }
    }

    libusb_free_device_list(deviceList, 1);
    return 0;
}

void reset() {
    // NOTE: context, libusb_init and libusb_exit should be refactored when
    //       moving this code to librealsense

    libusb_context *context;
    libusb_init(&context);
    libusb_device **deviceList = nullptr;

    ssize_t deviceCount = 0;
    deviceCount = libusb_get_device_list(context, &deviceList);

    for (size_t idx = 0; idx < deviceCount; ++idx) {
        libusb_device *device = deviceList[idx];
        libusb_device_descriptor desc = {0};

        int rc = libusb_get_device_descriptor(device, &desc);
        if (rc != LIBUSB_SUCCESS) {
            LOGE("Couldn't get device descriptor for USB device");
        }

        if (desc.idVendor == MOVIDIUS_VID && desc.idProduct == MOVIDIUS_PID) {
            int bus = libusb_get_bus_number(device);
            int port = libusb_get_port_number(device);

            LOGI("Found RealSense device %04x:%04x at bus: %d, port: %d",
                 desc.idVendor, desc.idProduct,
                 bus, port);

            libusb_device *hub = libusb_get_parent(device);
            libusb_device_descriptor hubDesc = {0};

            if (hub != nullptr) {
                rc = libusb_get_device_descriptor(hub, &hubDesc);
                if (rc == 0 && hubDesc.bDeviceClass == LIBUSB_CLASS_HUB) {
                    LOGI("Found parent hub %04x:%04x", hubDesc.idVendor, hubDesc.idProduct);
                } else {
                    LOGE("RealSense USB device parent %04x:%04x isn't classified as a hub! Perhaps the hub is buggy?!",
                         hubDesc.idVendor, hubDesc.idProduct);
                }
            } else {
                LOGE("Failed to get hub for %04x:%04x - won't be able to power cycle.", desc.idVendor, desc.idProduct);
                continue; // ignore this one.. :(
            }

            std::vector<libusb_device*>virtualHubs = {};
            rc = getVirtualHubs(context, hub, virtualHubs);
            if (rc < 0) {
                LOGE("Failed to get all virtual hubs for %04x:%04x - won't be able to power cycle.",
                        hubDesc.idVendor, hubDesc.idProduct);
            }

            for (int k = 0; k < 2; k ++) {
                int request = (k == 0) ? LIBUSB_REQUEST_CLEAR_FEATURE
                                       : LIBUSB_REQUEST_SET_FEATURE;


                // Handle USB3 duality mode - power off virtual 2.x hub & virtual 3.x hub
                // then turn them both on again..
                for (auto &hubDev: virtualHubs) {
                    struct libusb_device_handle * hubHandle = nullptr;

                    libusb_device_descriptor hubDevDesc = {0};
                    int rc = libusb_get_device_descriptor(hubDev, &hubDevDesc);
                    if (rc != LIBUSB_SUCCESS) {
                        LOGE("Couldn't get device descriptor for USB device");
                    }

                    rc = libusb_open(hubDev, &hubHandle);
                    if (rc != LIBUSB_SUCCESS) {
                        LOGE("Failed to open hub device %04x:%04x - permissions?", hubDevDesc.idVendor,
                             hubDevDesc.idProduct);
                        continue;
                    } else {
                        LOGD("Hub device %04x:%04x opened", hubDevDesc.idVendor, hubDevDesc.idProduct);
                    }

                    rc = libusb_control_transfer(hubHandle,
                                                 LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_OTHER,
                                                 request, USB_PORT_FEAT_POWER,
                                                 port, nullptr, 0, USB_CTRL_GET_TIMEOUT);

                    if (rc != LIBUSB_SUCCESS) {
                        LOGE("Failed to reset USB device %04x:%04x at bus: %d, port: %d",
                             desc.idVendor, desc.idProduct,
                             bus, port);
                        break;
                    } else {
                        LOGD("Port %d switched %s", port, k == 0 ? "OFF" : "ON");
                        std::this_thread::sleep_for(
                                std::chrono::milliseconds(1500));    // TODO: Figure out if we need more time here..
                    }
                    libusb_close(hubHandle);
                }
            }

        } else {
            // Noisy - turn on when needed...
            // LOGD("Skipping non-RealSense device %04x:%04x", desc.idVendor, desc.idProduct);
        }
    }

    libusb_free_device_list(deviceList, 1);
    libusb_exit(context);
}

int main(int argc, char *argv[])
{
    std::setbuf(stdout, NULL); // unbuffered stdout for our mock LOGx macros

    LOGI("TM265 USB Reset...");
    reset();
    LOGI("DONE...");
    return 0;
}
