/*
 * Copyright (c) 2024 ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * peripheral_status.c — Right (peripheral) half display.
 *
 * Shows battery level and whether it is connected to the central half.
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

/* ─── drawing ─────────────────────────────────────────────────────────────── */

static void draw_screen(lv_obj_t *widget, lv_color_t cbuf[],
                        const struct zmk_widget_peripheral_status *w) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_rect_dsc_t rect_bg, rect_fg, rect_bar;
    init_rect_dsc(&rect_bg,  LVGL_BACKGROUND);
    init_rect_dsc(&rect_fg,  LVGL_FOREGROUND);
    init_rect_dsc(&rect_bar, LVGL_FOREGROUND);

    lv_draw_label_dsc_t lbl_icon, lbl_text;
    init_label_dsc(&lbl_icon, LVGL_FOREGROUND, &lv_font_montserrat_26, LV_TEXT_ALIGN_CENTER);
    init_label_dsc(&lbl_text, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    /* ── connection icon centred ── */
    const char *icon = w->state.connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE;
    lv_canvas_draw_text(canvas, 0, 10, CANVAS_SIZE, &lbl_icon, icon);

    /* ── battery percentage ── */
    char batt_str[8];
    snprintf(batt_str, sizeof(batt_str), "%d%%", w->state.battery);
    lv_canvas_draw_text(canvas, 0, 48, CANVAS_SIZE, &lbl_text, batt_str);

    rotate_canvas(canvas, cbuf);
}

/* ─── battery listener ────────────────────────────────────────────────────── */

struct battery_status_state {
    uint8_t level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    bool usb_present;
#endif
};

static void battery_status_update_cb(struct battery_status_state st) {
    struct zmk_widget_peripheral_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        w->state.battery = st.level;
        draw_screen(w->obj, w->cbuf, w);
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

/* ─── connection listener ────────────────────────────────────────────────── */

struct peripheral_conn_state {
    bool connected;
};

static void conn_update_cb(struct peripheral_conn_state st) {
    struct zmk_widget_peripheral_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        w->state.connected = st.connected;
        draw_screen(w->obj, w->cbuf, w);
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

/* ─── public init ────────────────────────────────────────────────────────── */

int zmk_widget_peripheral_status_init(struct zmk_widget_peripheral_status *widget,
                                       lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    lv_obj_t *canvas = lv_canvas_create(widget->obj);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(canvas, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE,
                         LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);

    widget_battery_status_init();
    widget_peripheral_conn_init();

    return 0;
}

lv_obj_t *zmk_widget_peripheral_status_obj(struct zmk_widget_peripheral_status *widget) {
    return widget->obj;
}
