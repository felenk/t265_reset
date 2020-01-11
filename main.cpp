// Copyright(c) 2020 Intel Corporation. All Rights Reserved.

#include <stdio.h>
#include <iostream>
#include <libusb-1.0/libusb.h>
#include "log.h"
#include <chrono>
#include <thread>

using namespace std;

#define MOVIDIUS_VID 0x03e7
#define MOVIDIUS_PID 0x2150

#define USB_CTRL_GET_TIMEOUT     5000
#define USB_PORT_FEAT_POWER      (1 << 3)

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
        if (rc < 0) {
            LOGE("Couldn't get device descriptor for USB device");
        }

        if (desc.idVendor == MOVIDIUS_VID && desc.idProduct == MOVIDIUS_PID) {
            int bus = libusb_get_bus_number(device);
            int port = libusb_get_port_number(device);

            LOGI("Found RealSense %04x:%04x at bus: %d, port: %d",
                 desc.idVendor, desc.idProduct,
                 bus, port);

            libusb_device *hub = libusb_get_parent(device);
            libusb_device_descriptor hubDesc = {0};

            if (hub != nullptr) {
                rc = libusb_get_device_descriptor(hub, &hubDesc);
                if (rc == 0 && hubDesc.bDeviceClass == LIBUSB_CLASS_HUB) {
                    LOGI("Found parent hub %04x:%04x", hubDesc.idVendor, hubDesc.idProduct);
                } else {
                    LOGI("RealSense USB device parent %04x:%04x isn't classified as a hub! Perhaps the hub is buggy?!",
                         hubDesc.idVendor, hubDesc.idProduct);
                }
            } else {
                LOGE("Failed to get hub for %04x:%04x - won't be able to power cycle.", desc.idVendor, desc.idProduct);
                continue; // ignore this one.. :(
            }

            struct libusb_device_handle * hubHandle = nullptr;
            rc = libusb_open(hub, &hubHandle);
            if (rc < 0) {
                LOGE("Failed to open hub device %04x:%04x - permissions?", hubDesc.idVendor, hubDesc.idProduct);
                continue;
            } else{
                LOGD("Hub device %04x:%04x opened", hubDesc.idVendor, hubDesc.idProduct);
            }

            for (int k = 0; k < 2; k ++) {
                int request = (k == 0) ? LIBUSB_REQUEST_CLEAR_FEATURE
                                       : LIBUSB_REQUEST_SET_FEATURE;

                rc = libusb_control_transfer(hubHandle,
                                             LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_OTHER,
                                             request, USB_PORT_FEAT_POWER,
                                             port, nullptr, 0, USB_CTRL_GET_TIMEOUT);

                if (rc < 0) {
                    LOGE("Failed to reset USB device %04x:%04x at bus: %d, port: %d",
                         desc.idVendor, desc.idProduct,
                         bus, port);
                    break;
                } else {
                    LOGD("Port %d switched %s", port, k==0? "OFF": "ON");
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));    // TODO: Figure out if we need more time here..
                }
            }
            libusb_close(hubHandle);

        } else {
            // Noisy - turn on when needed...
            // LOGD("Skipping non-RealSense device %04x:%04x", desc.idVendor, desc.idProduct);
        }
    }
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
