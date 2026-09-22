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
    uint8_t     active_profile;   /* 0-4 */
    uint8_t     layer_index;
    const char *layer_label;
    char        key_str[8];
};

struct zmk_widget_central_status {
    sys_snode_t node;
    lv_obj_t   *obj;
    /* 4 canvases: each drawn 68w x 40h, rotated to 40w x 68h on screen */
    lv_color_t  cbuf_profile[CANVAS_W * CANVAS_H];
    lv_color_t  cbuf_battery[CANVAS_W * CANVAS_H];
    lv_color_t  cbuf_layer[CANVAS_W * CANVAS_H];
    lv_color_t  cbuf_key[CANVAS_W * CANVAS_H];
    struct central_status_state state;
};

int      zmk_widget_central_status_init(struct zmk_widget_central_status *widget,
                                         lv_obj_t *parent);
lv_obj_t *zmk_widget_central_status_obj(struct zmk_widget_central_status *widget);
