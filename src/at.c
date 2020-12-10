/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "at.h"
#include "suspend.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <glib-unix.h>

#define MODEM_UART "/dev/ttyS2"

struct AtCommand {
    char *cmd;
    char *subcmd;
    char *value;
    char *expected;
    int retries;
};

static int configure_serial(const char *tty)
{
    struct termios ttycfg;
    int fd;

    fd = open(tty, O_RDWR | O_NOCTTY);
    if (fd > 0) {
        tcgetattr(fd, &ttycfg);
        ttycfg.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | PARMRK | INPCK | ISTRIP | IXON);
        ttycfg.c_oflag = 0;
        ttycfg.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
        ttycfg.c_cflag &= ~(CSIZE | PARENB);
        ttycfg.c_cflag |= CS8;
        ttycfg.c_cc[VMIN] = 1;
        ttycfg.c_cc[VTIME] = 0;

        cfsetspeed(&ttycfg, B115200);
        tcsetattr(fd, TCSANOW, &ttycfg);
    }

    return fd;
}

static gboolean send_at_command(struct EG25Manager *manager)
{
    char command[256];
    struct AtCommand *at_cmd = g_list_nth_data(manager->at_cmds, 0);

    if (at_cmd) {
        if (at_cmd->subcmd == NULL && at_cmd->value == NULL)
            sprintf(command, "AT+%s?\r\n", at_cmd->cmd);
        else if (at_cmd->subcmd == NULL && at_cmd->value)
            sprintf(command, "AT+%s=%s\r\n", at_cmd->cmd, at_cmd->value);
        else if (at_cmd->subcmd && at_cmd->value == NULL)
            sprintf(command, "AT+%s=\"%s\"\r\n", at_cmd->cmd, at_cmd->subcmd);
        else if (at_cmd->subcmd && at_cmd->value)
            sprintf(command, "AT+%s=\"%s\",%s\r\n", at_cmd->cmd, at_cmd->subcmd, at_cmd->value);

        write(manager->at_fd, command, strlen(command));

        g_message("Sending command: %s", g_strstrip(command));
    } else if (manager->modem_state < EG25_STATE_CONFIGURED) {
        MMModemState modem_state = mm_modem_get_state(manager->mm_modem);

        if (manager->mm_modem && modem_state >= MM_MODEM_STATE_REGISTERED)
            modem_update_state(manager, modem_state);
        else
            manager->modem_state = EG25_STATE_CONFIGURED;
    } else if (manager->modem_state == EG25_STATE_SUSPENDING) {
        g_message("suspend sequence is over, drop inhibitor");
        suspend_inhibit(manager, FALSE);
    } else if (manager->modem_state == EG25_STATE_RESETTING) {
        manager->modem_state = EG25_STATE_POWERED;
    }

    return FALSE;
}

static void next_at_command(struct EG25Manager *manager)
{
    struct AtCommand *at_cmd = g_list_nth_data(manager->at_cmds, 0);

    if (at_cmd->cmd)
        g_free(at_cmd->cmd);
    if (at_cmd->subcmd)
        g_free(at_cmd->subcmd);
    if (at_cmd->value)
        g_free(at_cmd->value);
    if (at_cmd->expected)
        g_free(at_cmd->expected);
    g_free(at_cmd);
    manager->at_cmds = g_list_remove(manager->at_cmds, at_cmd);

    send_at_command(manager);
}

static void retry_at_command(struct EG25Manager *manager)
{
    struct AtCommand *at_cmd = g_list_nth_data(manager->at_cmds, 0);

    at_cmd->retries++;
    if (at_cmd->retries > 3) {
        g_critical("Command %s retried %d times, aborting...", at_cmd->cmd, at_cmd->retries);
        next_at_command(manager);
    } else {
        g_timeout_add_seconds(3, G_SOURCE_FUNC(send_at_command), manager);
    }
}

static void process_at_result(struct EG25Manager *manager, char *response)
{
    struct AtCommand *at_cmd = g_list_nth_data(manager->at_cmds, 0);

    if (at_cmd->expected && !strstr(response, at_cmd->expected)) {
        if (at_cmd->value)
            g_free(at_cmd->value);
        at_cmd->value = at_cmd->expected;
        at_cmd->expected = NULL;
        g_message("Got a different result than expected, changing value...");
        g_message("\t%s\n\t%s", at_cmd->expected, response);
        send_at_command(manager);
    } else {
        next_at_command(manager);
    }
}

static int append_at_command(struct EG25Manager *manager,
                             const char         *cmd,
                             const char         *subcmd,
                             const char         *value,
                             const char         *expected)
{
    struct AtCommand *at_cmd = calloc(1, sizeof(struct AtCommand));

    if (!at_cmd)
        return -1;

    at_cmd->retries = 0;
    at_cmd->cmd = g_strdup(cmd);
    if (subcmd)
        at_cmd->subcmd = g_strdup(subcmd);
    if (value)
        at_cmd->value = g_strdup(value);
    if (expected)
        at_cmd->expected = g_strdup(expected);

    manager->at_cmds = g_list_append(manager->at_cmds, at_cmd);

    return 0;
}

static gboolean modem_response(gint fd,
                               GIOCondition event,
                               gpointer data)
{
    struct EG25Manager *manager = data;
    char response[256];
    int ret;

    /*
     * TODO: several reads can be necessary to get the full response, we could
     * loop until we read 0 chars with a reasonable delay between attempts
     * (remember the transfer rate is 115200 here)
     */
    ret = read(fd, response, sizeof(response));
    if (ret > 0) {
        response[ret] = 0;
        g_strstrip(response);
        if (strlen(response) == 0)
            return TRUE;

        g_message("Response: [%s]", response);

        if (strcmp(response, "RDY") == 0)
            manager->modem_state = EG25_STATE_STARTED;
        else if (strcmp(response, "ERROR") == 0)
            retry_at_command(manager);
        else if (strstr(response, "OK"))
            process_at_result(manager, response);
    }

    return TRUE;
}

int at_init(struct EG25Manager *manager)
{
    manager->at_fd = configure_serial(MODEM_UART);
    if (manager->at_fd < 0) {
        g_critical("Unable to configure %s", MODEM_UART);
        return 1;
    }

    manager->at_source = g_unix_fd_add(manager->at_fd, G_IO_IN, modem_response, manager);

    return 0;
}

void at_destroy(struct EG25Manager *manager)
{
    g_source_remove(manager->at_source);
    if (manager->at_fd > 0)
        close(manager->at_fd);
}

void at_sequence_configure(struct EG25Manager *manager)
{
    /*
     * Default parameters in megi's driver which differ with our own:
     *   - urc/ri/* are always set the same way on both BH and CE
     *   - urc/ri/* pulse duration is 1 ms and urc/delay is 0 (no need to delay
     *     URCs if the pulse is that short)
     *   - apready is disabled
     *
     * Parameters set in megi's kernel but not here:
     *   - sleepind/level = 0
     *   - wakeupin/level = 0
     *   - ApRstLevel = 0
     *   - ModemRstLevel = 0
     *   - airplanecontrol = 1
     *   - fast/poweroff = 1
     * (those would need to be researched to check their usefulness for our
     * use-case)
     */

    append_at_command(manager, "QDAI", NULL, NULL, "1,1,0,1,0,0,1,1");
    append_at_command(manager, "QCFG", "risignaltype", NULL, "\"physical\"");
    append_at_command(manager, "QCFG", "ims", NULL, "1");
    if (manager->braveheart) {
        append_at_command(manager, "QCFG", "urc/ri/ring", NULL, "\"pulse\",2000,1000,5000,\"off\",1");
        append_at_command(manager, "QCFG", "urc/ri/smsincoming", NULL, "\"pulse\",2000");
        append_at_command(manager, "QCFG", "urc/ri/other", NULL, "\"off\",1");
        append_at_command(manager, "QCFG", "urc/delay", NULL, "1");
    } else {
        append_at_command(manager, "QCFG", "apready", NULL, "1,0,500");
    }
    append_at_command(manager, "QURCCFG", "urcport", NULL, "\"usbat\"");
    append_at_command(manager, "QGPS", NULL, NULL, "1");
    append_at_command(manager, "QSCLK", NULL, "1", NULL);
    send_at_command(manager);
}

void at_sequence_suspend(struct EG25Manager *manager)
{
    append_at_command(manager, "QGPS", NULL, NULL, "0");
    append_at_command(manager, "QCFG", "urc/cache", "1", NULL);
    send_at_command(manager);
}

void at_sequence_resume(struct EG25Manager *manager)
{
    append_at_command(manager, "QCFG", "urc/cache", "0", NULL);
    append_at_command(manager, "QGPS", NULL, NULL, "1");
    send_at_command(manager);
}

void at_sequence_reset(struct EG25Manager *manager)
{
    append_at_command(manager, "CFUN", NULL, "1,1", NULL);
    send_at_command(manager);
}
