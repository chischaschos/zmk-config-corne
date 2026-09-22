/*
 * Copyright (c) 2024 ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * central_status.c -- Left (central) half display.
 *
 * Layout (physical screen is 160x68, rotated 90 deg):
 * 4 tiles, each 68w x 40h drawn, rotated to 40w x 68h on screen.
 * From top to bottom as the user sees it:
 *   1. BT profile selector  (5 circles, active one filled)
 *   2. Battery bar + WiFi connection icon
 *   3. Layer name
 *   4. Last pressed key (shift-aware)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/battery.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include "util.h"
#include "central_status.h"

/* --- HID usage page / keycode ranges --- */
#define HID_USAGE_KEY       0x07
#define HID_USAGE_CONSUMER  0x0C

#define HID_KEY_A           0x04
#define HID_KEY_Z           0x1D
#define HID_KEY_1           0x1E
#define HID_KEY_0           0x27
#define HID_KEY_ENTER       0x28
#define HID_KEY_ESC         0x29
#define HID_KEY_BSPC        0x2A
#define HID_KEY_TAB         0x2B
#define HID_KEY_SPACE       0x2C
#define HID_KEY_MINUS       0x2D
#define HID_KEY_EQUAL       0x2E
#define HID_KEY_LBKT        0x2F
#define HID_KEY_RBKT        0x30
#define HID_KEY_BSLH        0x31
#define HID_KEY_SEMI        0x33
#define HID_KEY_SQT         0x34
#define HID_KEY_GRAVE       0x35
#define HID_KEY_COMMA       0x36
#define HID_KEY_DOT         0x37
#define HID_KEY_FSLH        0x38
#define HID_KEY_F1          0x3A
#define HID_KEY_F12         0x45
#define HID_KEY_RIGHT       0x4F
#define HID_KEY_LEFT        0x50
#define HID_KEY_DOWN        0x51
#define HID_KEY_UP          0x52
#define HID_KEY_DEL         0x4C
#define HID_KEY_HOME        0x4A
#define HID_KEY_END         0x4D
#define HID_KEY_PGUP        0x4B
#define HID_KEY_PGDN        0x4E
#define HID_KEY_LCTRL       0xE0
#define HID_KEY_LSHFT       0xE1
#define HID_KEY_LALT        0xE2
#define HID_KEY_LGUI        0xE3
#define HID_KEY_RCTRL       0xE4
#define HID_KEY_RSHFT       0xE5
#define HID_KEY_RALT        0xE6
#define HID_KEY_RGUI        0xE7

/* --- global modifier tracking --- */
static uint8_t held_mods = 0;

/* Map modifier keycodes to bit positions (HID modifier bitmap) */
static int mod_bit(uint32_t keycode) {
    if (keycode >= HID_KEY_LCTRL && keycode <= HID_KEY_RGUI) {
        return (int)(keycode - HID_KEY_LCTRL);
    }
    return -1;
}

/* --- keycode to display string --- */
static const char *keycode_to_str(uint16_t usage_page, uint32_t keycode,
                                   uint8_t explicit_mods) {
    if (usage_page != HID_USAGE_KEY) {
        return "???";
    }

    /* combine explicit modifiers (from the binding) with currently held mods */
    uint8_t mods = explicit_mods | held_mods;
    bool shift = (mods & (BIT(1) | BIT(5))) != 0; /* LSHFT | RSHFT */

    /* A-Z */
    if (keycode >= HID_KEY_A && keycode <= HID_KEY_Z) {
        static char buf[2];
        buf[0] = (char)((shift ? 'A' : 'a') + (keycode - HID_KEY_A));
        buf[1] = '\0';
        return buf;
    }

    /* 1-9, 0 */
    if (keycode >= HID_KEY_1 && keycode <= HID_KEY_0) {
        static const char *nums_plain[] = {"1","2","3","4","5","6","7","8","9","0"};
        static const char *nums_shift[] = {"!","@","#","$","%","^","&","*","(",")"};
        uint8_t idx = (keycode == HID_KEY_0) ? 9 : (keycode - HID_KEY_1);
        return shift ? nums_shift[idx] : nums_plain[idx];
    }

    /* F1-F12 */
    if (keycode >= HID_KEY_F1 && keycode <= HID_KEY_F12) {
        static const char *fkeys[] = {
            "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12"
        };
        return fkeys[keycode - HID_KEY_F1];
    }

    /* Special / punctuation */
    switch (keycode) {
    case HID_KEY_ENTER:  return "RET";
    case HID_KEY_ESC:    return "ESC";
    case HID_KEY_BSPC:   return "BSPC";
    case HID_KEY_TAB:    return "TAB";
    case HID_KEY_SPACE:  return "SPC";
    case HID_KEY_DEL:    return "DEL";
    case HID_KEY_HOME:   return "HOME";
    case HID_KEY_END:    return "END";
    case HID_KEY_PGUP:   return "PGUP";
    case HID_KEY_PGDN:   return "PGDN";
    case HID_KEY_UP:     return LV_SYMBOL_UP;
    case HID_KEY_DOWN:   return LV_SYMBOL_DOWN;
    case HID_KEY_LEFT:   return LV_SYMBOL_LEFT;
    case HID_KEY_RIGHT:  return LV_SYMBOL_RIGHT;
    case HID_KEY_LCTRL:
    case HID_KEY_RCTRL:  return "CTL";
    case HID_KEY_LSHFT:
    case HID_KEY_RSHFT:  return "SHF";
    case HID_KEY_LALT:
    case HID_KEY_RALT:   return "ALT";
    case HID_KEY_LGUI:
    case HID_KEY_RGUI:   return "GUI";
    case HID_KEY_MINUS:  return shift ? "_"  : "-";
    case HID_KEY_EQUAL:  return shift ? "+"  : "=";
    case HID_KEY_LBKT:   return shift ? "{"  : "[";
    case HID_KEY_RBKT:   return shift ? "}"  : "]";
    case HID_KEY_BSLH:   return shift ? "|"  : "\\";
    case HID_KEY_SEMI:   return shift ? ":"  : ";";
    case HID_KEY_SQT:    return shift ? "\"" : "'";
    case HID_KEY_GRAVE:  return shift ? "~"  : "`";
    case HID_KEY_COMMA:  return shift ? "<"  : ",";
    case HID_KEY_DOT:    return shift ? ">"  : ".";
    case HID_KEY_FSLH:   return shift ? "?"  : "/";
    default:             return "???";
    }
}

/* --- widget state --- */
static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* --- drawing helpers --- */

/*
 * Helper: set canvas to drawing dimensions (68w x 40h) before drawing.
 * rotate_canvas_rect will reconfigure to (40w x 68h) after.
 */
static void canvas_set_draw_mode(lv_obj_t *canvas, lv_color_t cbuf[]) {
    lv_canvas_set_buffer(canvas, cbuf, CANVAS_W, CANVAS_H, LV_IMG_CF_TRUE_COLOR);
}

/* Canvas indices (children of widget->obj):
 *   0 = profile (TOP_RIGHT  = top of rotated screen)
 *   1 = battery
 *   2 = layer
 *   3 = key     (TOP_LEFT   = bottom of rotated screen)
 */

/*
 * Panel 1: BT profile selector -- 5 circles, active one filled.
 */
static void draw_profile(lv_obj_t *widget, lv_color_t cbuf[],
                         const struct central_status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);
    canvas_set_draw_mode(canvas, cbuf);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_W, CANVAS_H, &rect_bg);

    /* 5 circles in two rows: row 1 = [1][2][3], row 2 = [4][5]
     * Circle diameter=16, gap=6 between circles, row gap=4.
     * Row 1: 3*16 + 2*6 = 60px wide, centered in 68.
     * Row 2: 2*16 + 1*6 = 38px wide, centered in 68.
     */
    const int circle_d = 16;
    const int gap = 6;
    const int row_gap = 4;
    const int row1_w = 3 * circle_d + 2 * gap;
    const int row2_w = 2 * circle_d + 1 * gap;
    const int row1_x = (CANVAS_W - row1_w) / 2;
    const int row2_x = (CANVAS_W - row2_w) / 2;
    const int total_h = 2 * circle_d + row_gap;
    const int row1_y = (CANVAS_H - total_h) / 2;
    const int row2_y = row1_y + circle_d + row_gap;

    lv_draw_rect_dsc_t circle_outline, circle_filled;
    init_rect_dsc(&circle_outline, LVGL_BACKGROUND);
    circle_outline.border_color = LVGL_FOREGROUND;
    circle_outline.border_width = 2;
    circle_outline.radius = circle_d / 2;

    init_rect_dsc(&circle_filled, LVGL_FOREGROUND);
    circle_filled.radius = circle_d / 2;

    lv_draw_label_dsc_t lbl_num;
    init_label_dsc(&lbl_num, LVGL_BACKGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    lv_draw_label_dsc_t lbl_num_inactive;
    init_label_dsc(&lbl_num_inactive, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    /* Positions: row, column offset for each of the 5 profiles */
    const int pos_x[] = {
        row1_x + 0 * (circle_d + gap),
        row1_x + 1 * (circle_d + gap),
        row1_x + 2 * (circle_d + gap),
        row2_x + 0 * (circle_d + gap),
        row2_x + 1 * (circle_d + gap),
    };
    const int pos_y[] = { row1_y, row1_y, row1_y, row2_y, row2_y };

    for (int i = 0; i < 5; i++) {
        bool active = (i == state->active_profile);

        if (active) {
            lv_canvas_draw_rect(canvas, pos_x[i], pos_y[i], circle_d, circle_d, &circle_filled);
        } else {
            lv_canvas_draw_rect(canvas, pos_x[i], pos_y[i], circle_d, circle_d, &circle_outline);
        }

        /* number centered in circle: font 14px in 16px circle, y offset +1 */
        char num[2] = { '1' + i, '\0' };
        lv_canvas_draw_text(canvas, pos_x[i], pos_y[i] + 1, circle_d,
                            active ? &lbl_num : &lbl_num_inactive, num);
    }

    rotate_canvas_rect(canvas, cbuf, CANVAS_W, CANVAS_H);
}

/*
 * Panel 2: Battery bar + connection icon.
 */
static void draw_battery(lv_obj_t *widget, lv_color_t cbuf[],
                         const struct central_status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);
    canvas_set_draw_mode(canvas, cbuf);

    lv_draw_rect_dsc_t rect_bg, rect_fg, rect_bar;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    init_rect_dsc(&rect_fg, LVGL_FOREGROUND);
    init_rect_dsc(&rect_bar, LVGL_FOREGROUND);

    lv_draw_label_dsc_t lbl_icon;
    init_label_dsc(&lbl_icon, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_W, CANVAS_H, &rect_bg);

    /* Battery bar vertically centered: y = (40-10)/2 = 15 */
    int bar_y = (CANVAS_H - 10) / 2;

    /* outline */
    lv_canvas_draw_rect(canvas, 2, bar_y, 44, 10, &rect_fg);
    lv_canvas_draw_rect(canvas, 3, bar_y + 1, 42, 8, &rect_bg);

    /* fill */
    uint8_t fill = (uint8_t)((state->battery * 40u + 50u) / 100u);
    if (fill > 0) {
        lv_canvas_draw_rect(canvas, 4, bar_y + 2, fill, 6, &rect_bar);
    }

    /* terminal nub */
    lv_canvas_draw_rect(canvas, 46, bar_y + 2, 4, 6, &rect_fg);
    lv_canvas_draw_rect(canvas, 47, bar_y + 3, 2, 4, &rect_bg);

    /* Connection icon to the right of battery */
    const char *icon = state->ble_bonded
        ? (state->ble_connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE)
        : LV_SYMBOL_SETTINGS;
    lv_canvas_draw_text(canvas, 52, bar_y - 2, CANVAS_W - 52, &lbl_icon, icon);

    rotate_canvas_rect(canvas, cbuf, CANVAS_W, CANVAS_H);
}

/*
 * Panel 3: Layer name.
 */
static void draw_layer(lv_obj_t *widget, lv_color_t cbuf[],
                       const struct central_status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 2);
    canvas_set_draw_mode(canvas, cbuf);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);

    lv_draw_label_dsc_t lbl_layer;
    init_label_dsc(&lbl_layer, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_W, CANVAS_H, &rect_bg);

    char layer_text[16] = {};
    if (state->layer_label != NULL && strlen(state->layer_label) > 0) {
        strncpy(layer_text, state->layer_label, sizeof(layer_text) - 1);
    } else {
        snprintf(layer_text, sizeof(layer_text), "L%d", state->layer_index);
    }

    /* center vertically: font ~14px, canvas 40px -> y = 13 */
    lv_canvas_draw_text(canvas, 0, 13, CANVAS_W, &lbl_layer, layer_text);

    rotate_canvas_rect(canvas, cbuf, CANVAS_W, CANVAS_H);
}

/*
 * Panel 4: Last pressed key, large and centered.
 */
static void draw_key(lv_obj_t *widget, lv_color_t cbuf[],
                     const struct central_status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 3);
    canvas_set_draw_mode(canvas, cbuf);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);

    lv_draw_label_dsc_t lbl_key;
    init_label_dsc(&lbl_key, LVGL_FOREGROUND, &lv_font_montserrat_26, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_W, CANVAS_H, &rect_bg);

    /* center vertically: font ~26px, canvas 40px -> y = 7 */
    lv_canvas_draw_text(canvas, 0, 7, CANVAS_W, &lbl_key, state->key_str);

    rotate_canvas_rect(canvas, cbuf, CANVAS_W, CANVAS_H);
}

/* --- battery listener --- */

static void battery_status_update_cb(struct battery_status_state st) {
    struct zmk_widget_central_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        w->state.battery = st.level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        w->state.charging = st.usb_present;
#endif
        draw_battery(w->obj, w->cbuf_battery, &w->state);
    }
}

static struct battery_status_state battery_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
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

/* --- output (BLE/USB) listener --- */

struct output_status_state {
    bool ble_connected;
    bool ble_bonded;
    uint8_t active_profile;
};

static void output_status_update_cb(struct output_status_state st) {
    struct zmk_widget_central_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        w->state.ble_connected   = st.ble_connected;
        w->state.ble_bonded      = st.ble_bonded;
        w->state.active_profile  = st.active_profile;
        draw_battery(w->obj, w->cbuf_battery, &w->state);
        draw_profile(w->obj, w->cbuf_profile, &w->state);
    }
}

static struct output_status_state output_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .ble_connected  = zmk_ble_active_profile_is_connected(),
        .ble_bonded     = !zmk_ble_active_profile_is_open(),
        .active_profile = zmk_ble_active_profile_index(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

/* --- layer listener --- */

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

static void layer_status_update_cb(struct layer_status_state st) {
    struct zmk_widget_central_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        w->state.layer_index = st.index;
        w->state.layer_label = st.label;
        draw_layer(w->obj, w->cbuf_layer, &w->state);
    }
}

static struct layer_status_state layer_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index,
        .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state,
                            layer_status_update_cb, layer_get_state)
ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

/* --- key-press listener --- */

struct key_press_state {
    uint16_t usage_page;
    uint32_t keycode;
    uint8_t  mods;
    bool     pressed;
};

static void key_press_update_cb(struct key_press_state st) {
    /* Track held modifier keys globally */
    if (st.usage_page == HID_USAGE_KEY) {
        int bit = mod_bit(st.keycode);
        if (bit >= 0) {
            if (st.pressed) {
                held_mods |= BIT(bit);
            } else {
                held_mods &= ~BIT(bit);
            }
        }
    }

    if (!st.pressed) {
        return; /* only update display on key-down */
    }

    struct zmk_widget_central_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        const char *s = keycode_to_str(st.usage_page, st.keycode, st.mods);
        strncpy(w->state.key_str, s, sizeof(w->state.key_str) - 1);
        w->state.key_str[sizeof(w->state.key_str) - 1] = '\0';
        draw_key(w->obj, w->cbuf_key, &w->state);
    }
}

static struct key_press_state key_press_get_state(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL) {
        return (struct key_press_state){0};
    }
    return (struct key_press_state){
        .usage_page = ev->usage_page,
        .keycode    = ev->keycode,
        .mods       = ev->explicit_modifiers,
        .pressed    = ev->state,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_key_press, struct key_press_state,
                            key_press_update_cb, key_press_get_state)
ZMK_SUBSCRIPTION(widget_key_press, zmk_keycode_state_changed);

/* --- public init --- */

int zmk_widget_central_status_init(struct zmk_widget_central_status *widget,
                                    lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    /* 4 canvases: each drawn as 68w x 40h, then rotated to 40w x 68h.
     *
     * On the 160x68 lv_obj, placed at:
     *   canvas 0 (profile): x=120  (rightmost = top of user view)
     *   canvas 1 (battery): x=80
     *   canvas 2 (layer):   x=40
     *   canvas 3 (key):     x=0    (leftmost = bottom of user view)
     *
     * Initially set to drawing dimensions (68w x 40h).
     * Each draw function resets to draw mode, draws, then rotate_canvas_rect
     * reconfigures to display mode (40w x 68h).
     */

    lv_obj_t *c0 = lv_canvas_create(widget->obj);
    lv_obj_set_pos(c0, 120, 0);
    lv_canvas_set_buffer(c0, widget->cbuf_profile, CANVAS_W, CANVAS_H,
                         LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *c1 = lv_canvas_create(widget->obj);
    lv_obj_set_pos(c1, 80, 0);
    lv_canvas_set_buffer(c1, widget->cbuf_battery, CANVAS_W, CANVAS_H,
                         LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *c2 = lv_canvas_create(widget->obj);
    lv_obj_set_pos(c2, 40, 0);
    lv_canvas_set_buffer(c2, widget->cbuf_layer, CANVAS_W, CANVAS_H,
                         LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *c3 = lv_canvas_create(widget->obj);
    lv_obj_set_pos(c3, 0, 0);
    lv_canvas_set_buffer(c3, widget->cbuf_key, CANVAS_W, CANVAS_H,
                         LV_IMG_CF_TRUE_COLOR);

    /* Initial state */
    strncpy(widget->state.key_str, "--", sizeof(widget->state.key_str));

    sys_slist_append(&widgets, &widget->node);

    widget_battery_status_init();
    widget_output_status_init();
    widget_layer_status_init();
    widget_key_press_init();

    return 0;
}

lv_obj_t *zmk_widget_central_status_obj(struct zmk_widget_central_status *widget) {
    return widget->obj;
}
