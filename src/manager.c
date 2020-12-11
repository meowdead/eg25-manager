/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "at.h"
#include "gpio.h"
#include "manager.h"
#include "mm-iface.h"
#include "suspend.h"

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <glib-unix.h>
#include <libusb.h>

#define EG25_USB_VID 0x2c7c
#define EG25_USB_PID 0x0125

static gboolean quit_app(struct EG25Manager *manager)
{
    int i;

    g_message("Request to quit...");

    at_destroy(manager);
    mm_iface_destroy(manager);
    suspend_destroy(manager);

    if (manager->modem_state >= EG25_STATE_STARTED) {
        g_message("Powering down the modem...");
        gpio_sequence_shutdown(manager);
        manager->modem_state = EG25_STATE_FINISHING;
        for (i = 0; i < 30; i++) {
            if (gpio_check_poweroff(manager, TRUE))
                break;
            sleep(1);
        }
    }
    g_message("Modem down, quitting...");

    g_main_loop_quit(manager->loop);

    return FALSE;
}

static gboolean modem_start(struct EG25Manager *manager)
{
    ssize_t i, count;
    gboolean should_boot = TRUE;
    libusb_context *ctx = NULL;
    libusb_device **devices = NULL;
    struct libusb_device_descriptor desc;

    if (manager->braveheart) {
        // BH don't have the STATUS line connected, so check if USB device is present
        libusb_init(&ctx);

        count = libusb_get_device_list(ctx, &devices);
        for (i = 0; i < count; i++) {
            libusb_get_device_descriptor(devices[i], &desc);
            if (desc.idVendor == EG25_USB_VID && desc.idProduct == EG25_USB_PID) {
                g_message("Found corresponding USB device, modem already powered");
                should_boot = FALSE;
                break;
            }
        }

        libusb_free_device_list(devices, 1);
        libusb_exit(ctx);
    } else if (!gpio_check_poweroff(manager, FALSE)) {
        g_message("STATUS is low, modem already powered");
        should_boot = FALSE;
    }

    if (should_boot) {
        g_message("Starting modem...");
        gpio_sequence_poweron(manager);
        manager->modem_state = EG25_STATE_POWERED;
    } else {
        manager->modem_state = EG25_STATE_STARTED;
    }

    return FALSE;
}

void modem_update_state(struct EG25Manager *manager, MMModemState state)
{
    switch (state) {
    case MM_MODEM_STATE_REGISTERED:
    case MM_MODEM_STATE_DISCONNECTING:
    case MM_MODEM_STATE_CONNECTING:
        manager->modem_state = EG25_STATE_REGISTERED;
        break;
    case MM_MODEM_STATE_CONNECTED:
        manager->modem_state = EG25_STATE_CONNECTED;
        break;
    default:
        manager->modem_state = EG25_STATE_CONFIGURED;
        break;
    }
}

void modem_configure(struct EG25Manager *manager)
{
    at_sequence_configure(manager);
}

void modem_reset(struct EG25Manager *manager)
{
    int fd;

    fd = open("/sys/bus/usb/drivers/usb/unbind", O_WRONLY);
    if (fd < 0)
        goto error;
    write(fd, manager->modem_usb_id, strlen(manager->modem_usb_id));
    close(fd);

    fd = open("/sys/bus/usb/drivers/usb/bind", O_WRONLY);
    if (fd < 0)
        goto error;
    write(fd, manager->modem_usb_id, strlen(manager->modem_usb_id));
    close(fd);

    manager->modem_state = EG25_STATE_CONFIGURED;
    return;

error:
    // Everything else failed, reset the modem
    at_sequence_reset(manager);
    manager->modem_state = EG25_STATE_RESETTING;
}

void modem_suspend_pre(struct EG25Manager *manager)
{
    manager->modem_state = EG25_STATE_SUSPENDING;
    gpio_sequence_suspend(manager);
}

void modem_suspend_post(struct EG25Manager *manager)
{
    at_sequence_suspend(manager);
}

void modem_resume_pre(struct EG25Manager *manager)
{
    manager->modem_state = EG25_STATE_RESUMING;
    gpio_sequence_resume(manager);
}

void modem_resume_post(struct EG25Manager *manager)
{
    at_sequence_resume(manager);
}

int main(int argc, char *argv[])
{
    struct EG25Manager manager;
    char compatible[32];
    int fd;

    memset(&manager, 0, sizeof(manager));
    manager.at_fd = -1;
    manager.suspend_inhibit_fd = -1;

    manager.loop = g_main_loop_new(NULL, FALSE);

    fd = open("/proc/device-tree/compatible", O_RDONLY);
    if (fd < 0) {
        g_critical("Unable to read 'compatible' string from device tree");
        return 1;
    }
    read(fd, compatible, sizeof(compatible));
    if (!strstr(compatible, "pine64,pinephone-1.2"))
        manager.braveheart = TRUE;
    close(fd);

    at_init(&manager);
    gpio_init(&manager);
    mm_iface_init(&manager);
    suspend_init(&manager);

    g_idle_add(G_SOURCE_FUNC(modem_start), &manager);

    g_unix_signal_add(SIGINT, G_SOURCE_FUNC(quit_app), &manager);
    g_unix_signal_add(SIGTERM, G_SOURCE_FUNC(quit_app), &manager);

    g_main_loop_run(manager.loop);

    gpio_destroy(&manager);

    return 0;
}
