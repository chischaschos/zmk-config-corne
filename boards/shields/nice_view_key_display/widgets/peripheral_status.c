/*
 * Copyright (c) 2024 ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * peripheral_status.c -- Right (peripheral) half display.
 *
 * Layout (2 square 68x68 canvases):
 *   Top:    battery bar + connection icon
 *   Bottom: keyboard name
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/battery.h>
#include <zmk/usb.h>
#include <zmk/split/bluetooth/peripheral.h>

#include "util.h"
#include "peripheral_status.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* --- drawing --- */

/*
 * Top canvas: battery bar + WiFi icon, centered vertically.
 */
static void draw_top(lv_obj_t *widget, lv_color_t cbuf[],
                     const struct zmk_widget_peripheral_status *w) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_rect_dsc_t rect_bg, rect_fg, rect_bar;
    init_rect_dsc(&rect_bg,  LVGL_BACKGROUND);
    init_rect_dsc(&rect_fg,  LVGL_FOREGROUND);
    init_rect_dsc(&rect_bar, LVGL_FOREGROUND);

    lv_draw_label_dsc_t lbl_icon;
    init_label_dsc(&lbl_icon, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    /* Battery bar centered vertically: y = (68-10)/2 = 29 */
    int bar_y = (CANVAS_SIZE - 10) / 2;

    /* outline */
    lv_canvas_draw_rect(canvas, 2, bar_y, 44, 10, &rect_fg);
    lv_canvas_draw_rect(canvas, 3, bar_y + 1, 42, 8, &rect_bg);

    /* fill */
    uint8_t fill = (uint8_t)((w->state.battery * 40u + 50u) / 100u);
    if (fill > 0) {
        lv_canvas_draw_rect(canvas, 4, bar_y + 2, fill, 6, &rect_bar);
    }

    /* terminal nub */
    lv_canvas_draw_rect(canvas, 46, bar_y + 2, 4, 6, &rect_fg);
    lv_canvas_draw_rect(canvas, 47, bar_y + 3, 2, 4, &rect_bg);

    /* Connection icon to the right of battery */
    const char *icon = w->state.connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE;
    lv_canvas_draw_text(canvas, 52, bar_y - 2, CANVAS_SIZE - 52, &lbl_icon, icon);

    rotate_canvas(canvas, cbuf);
}

/*
 * Bottom canvas: keyboard name, centered.
 */
static void draw_bottom(lv_obj_t *widget, lv_color_t cbuf[]) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);

    lv_draw_label_dsc_t lbl_name;
    init_label_dsc(&lbl_name, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    /* center vertically: font ~14px, canvas 68px -> y = 27 */
    lv_canvas_draw_text(canvas, 0, 27, CANVAS_SIZE, &lbl_name, CONFIG_ZMK_KEYBOARD_NAME);

    rotate_canvas(canvas, cbuf);
}

/* --- battery listener --- */

static void battery_status_update_cb(struct battery_status_state st) {
    struct zmk_widget_peripheral_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        w->state.battery = st.level;
        draw_top(w->obj, w->cbuf_top, w);
    }
}

static struct battery_status_state battery_get_state(const zmk_event_t *eh) {
    return (struct battery_status_state){
        .level = zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_get_state)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif

/* --- connection listener --- */

struct peripheral_conn_state {
    bool connected;
};

static void conn_update_cb(struct peripheral_conn_state st) {
    struct zmk_widget_peripheral_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        w->state.connected = st.connected;
        draw_top(w->obj, w->cbuf_top, w);
    }
}

static struct peripheral_conn_state conn_get_state(const zmk_event_t *_eh) {
    return (struct peripheral_conn_state){
        .connected = zmk_split_bt_peripheral_is_connected(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_conn, struct peripheral_conn_state,
                            conn_update_cb, conn_get_state)
ZMK_SUBSCRIPTION(widget_peripheral_conn, zmk_split_peripheral_status_changed);

/* --- public init --- */

int zmk_widget_peripheral_status_init(struct zmk_widget_peripheral_status *widget,
                                       lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    /* Top canvas (right side of 160x68 lv_obj = top of rotated screen) */
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf_top, CANVAS_SIZE, CANVAS_SIZE,
                         LV_IMG_CF_TRUE_COLOR);

    /* Bottom canvas (left side = bottom of rotated screen) */
    lv_obj_t *bot = lv_canvas_create(widget->obj);
    lv_obj_align(bot, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(bot, widget->cbuf_bot, CANVAS_SIZE, CANVAS_SIZE,
                         LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);

    widget_battery_status_init();
    widget_peripheral_conn_init();

    /* Draw the static bottom panel (keyboard name) */
    draw_bottom(widget->obj, widget->cbuf_bot);

    return 0;
}

lv_obj_t *zmk_widget_peripheral_status_obj(struct zmk_widget_peripheral_status *widget) {
    return widget->obj;
}
