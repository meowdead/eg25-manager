/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "udev.h"

#include <string.h>

static void udev_event_cb(GUdevClient *client, gchar *action, GUdevDevice *device, gpointer data)
{
    struct EG25Manager *manager = data;

    if (strcmp(action, "unbind") != 0 || manager->modem_state == EG25_STATE_RESETTING)
        return;

    if (strncmp(g_udev_device_get_name(device), manager->modem_usb_id, strlen(manager->modem_usb_id)) == 0 &&
        manager->reset_timer == 0) {
        g_message("Lost modem, resetting...");
        modem_reset(manager);
    }
}

void udev_init (struct EG25Manager *manager)
{
    const char * const subsystems[] = { "usb", NULL };

    manager->udev = g_udev_client_new(subsystems);
    g_signal_connect(manager->udev, "uevent", G_CALLBACK(udev_event_cb), manager);

    return;
}

void udev_destroy (struct EG25Manager *manager)
{
    if (manager->udev) {
        g_object_unref(manager->udev);
        manager->udev = NULL;
    }
}
