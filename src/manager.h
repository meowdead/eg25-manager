/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>
#include <gpiod.h>
#include <gudev/gudev.h>
#include <libmm-glib.h>

enum EG25State {
    EG25_STATE_INIT = 0,
    EG25_STATE_POWERED, // Power-on sequence has been executed, but the modem isn't on yet
    EG25_STATE_STARTED, // Modem has been started and declared itdata ready
    EG25_STATE_ACQUIRED, // Modem has been probed by ModemManager
    EG25_STATE_CONFIGURED, // Modem has been configured through AT commands
    EG25_STATE_SUSPENDING, // System is going into suspend
    EG25_STATE_RESUMING, // System is being resumed, waiting for modem to come back
    EG25_STATE_REGISTERED, // Modem is unlocked and registered to a network provider
    EG25_STATE_CONNECTED, // Modem is connected (data connection active)
    EG25_STATE_RESETTING, // Something went wrong, we're restarting the modem
    EG25_STATE_FINISHING
};

struct EG25Manager {
    GMainLoop *loop;
    guint reset_timer;

    int at_fd;
    guint at_source;
    GList *at_cmds;

    enum EG25State modem_state;
    gchar *modem_usb_id;
    gboolean braveheart;

    guint mm_watch;
    MMManager *mm_manager;
    MMModem *mm_modem;

    GDBusProxy *suspend_proxy;
    int suspend_inhibit_fd;
    guint suspend_timer;

    GUdevClient *udev;

    struct gpiod_chip *gpiochip[2];
    struct gpiod_line *gpio_out[5];
    struct gpiod_line *gpio_in[2];
};

void modem_configure(struct EG25Manager *data);
void modem_reset(struct EG25Manager *data);
void modem_suspend(struct EG25Manager *data);
void modem_resume_pre(struct EG25Manager *data);
void modem_resume_post(struct EG25Manager *data);
void modem_update_state(struct EG25Manager *data, MMModemState state);
