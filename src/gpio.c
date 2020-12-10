/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpio.h"

#define GPIO_CHIP1_LABEL "1c20800.pinctrl"
#define GPIO_CHIP2_LABEL "1f02c00.pinctrl"

#define MAX_GPIOCHIP_LINES 352

#define GPIO_IDX_INVAL 0xffff

enum {
    GPIO_OUT_DTR = 0,
    GPIO_OUT_PWRKEY,
    GPIO_OUT_RESET,
    GPIO_OUT_APREADY,
    GPIO_OUT_DISABLE,
    GPIO_OUT_COUNT
};

static unsigned gpio_out_idx_bh[] = {
    358,
    35,
    68,
    231,
    232
};

static unsigned gpio_out_idx_ce[] = {
    34,
    35,
    68,
    231,
    232
};

enum {
    GPIO_IN_STATUS = 0,
    GPIO_IN_COUNT
};

static unsigned gpio_in_idx_bh[] = {
    GPIO_IDX_INVAL,
};

static unsigned gpio_in_idx_ce[] = {
    233,
};

int gpio_sequence_poweron(struct EG25Manager *manager)
{
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_PWRKEY], 1);
    sleep(1);
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_PWRKEY], 0);

    g_message("Executed power-on sequence");

    return 0;
}

int gpio_sequence_shutdown(struct EG25Manager *manager)
{
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_DISABLE], 1);
    gpio_sequence_poweron(manager);

    g_message("Executed power-off sequence");

    return 0;
}

int gpio_sequence_suspend(struct EG25Manager *manager)
{
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_APREADY], 1);
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_DTR], 1);

    g_message("Executed suspend sequence");

    return 0;
}

int gpio_sequence_resume(struct EG25Manager *manager)
{
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_APREADY], 0);
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_DTR], 0);

    g_message("Executed resume sequence");

    return 0;
}

int gpio_init(struct EG25Manager *manager)
{
    int i, ret;
    unsigned *gpio_in_idx;
    unsigned *gpio_out_idx;

    manager->gpiochip[0] = gpiod_chip_open_by_label(GPIO_CHIP1_LABEL);
    if (!manager->gpiochip[0]) {
        g_critical("Unable to open GPIO chip " GPIO_CHIP1_LABEL);
        return 1;
    }

    manager->gpiochip[1] = gpiod_chip_open_by_label(GPIO_CHIP2_LABEL);
    if (!manager->gpiochip[1]) {
        g_critical("Unable to open GPIO chip " GPIO_CHIP2_LABEL);
        return 1;
    }

    if (manager->braveheart) {
        gpio_in_idx = gpio_in_idx_bh;
        gpio_out_idx = gpio_out_idx_bh;
    } else {
        gpio_in_idx = gpio_in_idx_ce;
        gpio_out_idx = gpio_out_idx_ce;
    }

    for (i = 0; i < GPIO_OUT_COUNT; i++) {
        unsigned int offset, chipidx;

        if (gpio_out_idx[i] < MAX_GPIOCHIP_LINES) {
            offset = gpio_out_idx[i];
            chipidx = 0;
        } else {
            offset = gpio_out_idx[i] - MAX_GPIOCHIP_LINES;
            chipidx = 1;
        }

        manager->gpio_out[i] = gpiod_chip_get_line(manager->gpiochip[chipidx], offset);
        if (!manager->gpio_out[i]) {
            g_critical("Unable to get output GPIO %d", i);
            return 1;
        }

        ret = gpiod_line_request_output(manager->gpio_out[i], "eg25manager", 0);
        if (ret < 0) {
            g_critical("Unable to request output GPIO %d", i);
            return 1;
        }
    }

    for (i = 0; i < GPIO_IN_COUNT; i++) {
        unsigned int offset, chipidx;

        if (gpio_in_idx[i] == GPIO_IDX_INVAL)
            continue;

        if (gpio_in_idx[i] < MAX_GPIOCHIP_LINES) {
            offset = gpio_in_idx[i];
            chipidx = 0;
        } else {
            offset = gpio_in_idx[i] - MAX_GPIOCHIP_LINES;
            chipidx = 1;
        }

        manager->gpio_in[i] = gpiod_chip_get_line(manager->gpiochip[chipidx], offset);
        if (!manager->gpio_in[i]) {
            g_warning("Unable to get input GPIO %d", i);
            continue;
        }

        ret = gpiod_line_request_input(manager->gpio_in[i], "eg25manager");
        if (ret < 0) {
            g_warning("Unable to request input GPIO %d", i);
            manager->gpio_in[i] = NULL;
        }
    }

    return 0;
}

gboolean gpio_check_poweroff(struct EG25Manager *manager)
{
    if (manager->gpio_in[GPIO_IN_STATUS] &&
        gpiod_line_get_value(manager->gpio_in[GPIO_IN_STATUS]) == 1) {

        // Asserting RESET line to prevent modem from rebooting
        gpiod_line_set_value(manager->gpio_out[GPIO_OUT_RESET], 1);
        return TRUE;
    }

    return FALSE;
}

void gpio_destroy(struct EG25Manager *manager)
{
    int i;

    for (i = 0; i < GPIO_OUT_COUNT; i++)
        gpiod_line_release(manager->gpio_out[i]);

    gpiod_chip_close(manager->gpiochip[0]);
    if (manager->gpiochip[1])
        gpiod_chip_close(manager->gpiochip[1]);
}
