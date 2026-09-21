/*
 * Copyright (c) 2024 ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>
#include "util.h"

struct central_status_state {
    uint8_t     battery;
    bool        charging;
    bool        ble_connected;
    bool        ble_bonded;
    uint8_t     layer_index;
    const char *layer_label;
    char        key_str[8];
};

struct zmk_widget_central_status {
    sys_snode_t node;
    lv_obj_t   *obj;
    lv_color_t  cbuf_top[CANVAS_SIZE * CANVAS_SIZE];
    lv_color_t  cbuf_bot[CANVAS_SIZE * CANVAS_SIZE];
    struct central_status_state state;
};

int      zmk_widget_central_status_init(struct zmk_widget_central_status *widget,
                                         lv_obj_t *parent);
lv_obj_t *zmk_widget_central_status_obj(struct zmk_widget_central_status *widget);
