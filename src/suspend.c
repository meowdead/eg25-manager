/*
 * Copyright (C) 2012 Red Hat, Inc.
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * Copied and adapted from ModemManager's `mm-sleep-monitor.c`:
 *   https://gitlab.freedesktop.org/mobile-broadband/ModemManager/
 * Author: Matthias Clasen <mclasen@redhat.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "manager.h"

#include <gio/gunixfdlist.h>

#define SD_NAME      "org.freedesktop.login1"
#define SD_PATH      "/org/freedesktop/login1"
#define SD_INTERFACE "org.freedesktop.login1.Manager"

static gboolean check_modem_resume(struct EG25Manager *manager)
{
    g_message("Modem wasn't probed in time, restart it!");
    modem_reset(manager);

    return FALSE;
}

static gboolean drop_inhibitor(struct EG25Manager *manager)
{
    if (manager->suspend_inhibit_fd >= 0) {
        g_message("dropping systemd sleep inhibitor");
        close(manager->suspend_inhibit_fd);
        manager->suspend_inhibit_fd = -1;
        return TRUE;
    }
    return FALSE;
}

static void inhibit_done(GObject *source,
                         GAsyncResult *result,
                         gpointer user_data)
{
    GDBusProxy *suspend_proxy = G_DBUS_PROXY(source);
    struct EG25Manager *manager = user_data;
    g_autoptr (GError) error = NULL;
    GVariant *res;
    GUnixFDList *fd_list;

    res = g_dbus_proxy_call_with_unix_fd_list_finish(suspend_proxy, &fd_list, result, &error);
    if (!res) {
        g_warning("inhibit failed: %s", error->message);
    } else {
        if (!fd_list || g_unix_fd_list_get_length(fd_list) != 1)
            g_warning("didn't get a single fd back");

        manager->suspend_inhibit_fd = g_unix_fd_list_get(fd_list, 0, NULL);

        g_message("inhibitor fd is %d", manager->suspend_inhibit_fd);
        g_object_unref(fd_list);
        g_variant_unref(res);
    }
}

static void take_inhibitor(struct EG25Manager *manager)
{
    GVariant *variant_arg;

    if(manager->suspend_inhibit_fd != -1)
        drop_inhibitor(manager);

    variant_arg = g_variant_new ("(ssss)", "sleep", "eg25manager",
                                 "eg25manager needs to prepare modem for sleep", "delay");

    g_message("taking systemd sleep inhibitor");
    g_dbus_proxy_call_with_unix_fd_list(manager->suspend_proxy, "Inhibit", variant_arg,
                                        0, G_MAXINT, NULL, NULL, inhibit_done, manager);
}

static void signal_cb(GDBusProxy *proxy,
                      const gchar *sendername,
                      const gchar *signalname,
                      GVariant *args,
                      gpointer user_data)
{
    struct EG25Manager *manager = user_data;
    gboolean is_about_to_suspend;

    if (strcmp(signalname, "PrepareForSleep") != 0)
        return;

    g_variant_get(args, "(b)", &is_about_to_suspend);

    if (is_about_to_suspend) {
        g_message("system is about to suspend");
        manager->modem_state = EG25_STATE_SUSPENDING;
        modem_suspend(manager);
    } else {
        g_message("system is resuming");
        take_inhibitor(manager);
        modem_resume_pre(manager);
        manager->modem_state = EG25_STATE_RESUMING;
        manager->suspend_source = g_timeout_add_seconds(8, G_SOURCE_FUNC(check_modem_resume), manager);
    }
}

static void name_owner_cb(GObject *object,
                          GParamSpec *pspec,
                          gpointer user_data)
{
    GDBusProxy *proxy = G_DBUS_PROXY(object);
    struct EG25Manager *manager = user_data;
    char *owner;

    g_assert(proxy == manager->suspend_proxy);

    owner = g_dbus_proxy_get_name_owner(proxy);
    if (owner) {
        take_inhibitor(manager);
        g_free(owner);
    } else {
        drop_inhibitor(manager);
    }
}

static void on_proxy_acquired(GObject *object,
                              GAsyncResult *res,
                              struct EG25Manager *manager)
{
    g_autoptr (GError) error = NULL;
    char *owner;

    manager->suspend_proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (!manager->suspend_proxy) {
        g_warning("failed to acquire logind proxy: %s", error->message);
        return;
    }

    g_signal_connect(manager->suspend_proxy, "notify::g-name-owner", G_CALLBACK(name_owner_cb), manager);
    g_signal_connect(manager->suspend_proxy, "g-signal", G_CALLBACK(signal_cb), manager);

    owner = g_dbus_proxy_get_name_owner(manager->suspend_proxy);
    if (owner) {
        take_inhibitor(manager);
        g_free(owner);
    }
}

void suspend_init(struct EG25Manager *manager)
{
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
                             G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START |
                             G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                             NULL, SD_NAME, SD_PATH, SD_INTERFACE, NULL,
                             (GAsyncReadyCallback)on_proxy_acquired, manager);
}

void suspend_destroy(struct EG25Manager *manager)
{
    drop_inhibitor(manager);
    if (manager->suspend_proxy) {
        g_object_unref(manager->suspend_proxy);
        manager->suspend_proxy = NULL;
    }
}

void suspend_inhibit(struct EG25Manager *manager, gboolean inhibit)
{
    if (inhibit)
        take_inhibitor(manager);
    else
        drop_inhibitor(manager);
}
