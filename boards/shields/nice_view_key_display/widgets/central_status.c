/*
 * Copyright (c) 2024 ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * central_status.c — Left (central) half display.
 *
 * Layout (physical screen is 160 x 68 px, rotated):
 *   Top canvas    (68x68): current layer name + BT/USB icon + battery
 *   Bottom canvas (68x68): last pressed key — displayed BIG in the centre
 *
 * The two 68x68 tiles are each drawn "upright" and then rotated 90° so they
 * appear correctly on the rotated display hardware.
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

/* ─── HID usage page / keycode ranges ─────────────────────────────────────── */
#define HID_USAGE_KEY       0x07
#define HID_USAGE_CONSUMER  0x0C

/* HID keyboard keycodes (usage page 0x07) */
#define HID_KEY_A           0x04
#define HID_KEY_Z           0x1D
#define HID_KEY_1           0x1E
#define HID_KEY_0           0x27   /* HID 0x27 = '0' key */
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
#define HID_KEY_LCTRL       0xE0
#define HID_KEY_LSHFT       0xE1
#define HID_KEY_LALT        0xE2
#define HID_KEY_LGUI        0xE3
#define HID_KEY_RCTRL       0xE4
#define HID_KEY_RSHFT       0xE5
#define HID_KEY_RALT        0xE6
#define HID_KEY_RGUI        0xE7

/* ─── keycode → display string ─────────────────────────────────────────────── */
static const char *keycode_to_str(uint16_t usage_page, uint32_t keycode,
                                   uint8_t mods) {
    if (usage_page != HID_USAGE_KEY) {
        return "???";
    }

    bool shift = (mods & (BIT(1) | BIT(5))) != 0; /* LSHFT | RSHFT */

    /* A–Z */
    if (keycode >= HID_KEY_A && keycode <= HID_KEY_Z) {
        static char buf[2];
        buf[0] = (char)((shift ? 'A' : 'a') + (keycode - HID_KEY_A));
        buf[1] = '\0';
        return buf;
    }

    /* 1–9, 0 */
    if (keycode >= HID_KEY_1 && keycode <= HID_KEY_0) {
        static const char *nums_plain[] = {"1","2","3","4","5","6","7","8","9","0"};
        static const char *nums_shift[] = {"!","@","#","$","%","^","&","*","(",")"};
        uint8_t idx = (keycode == HID_KEY_0) ? 9 : (keycode - HID_KEY_1);
        return shift ? nums_shift[idx] : nums_plain[idx];
    }

    /* F1–F12 */
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

/* ─── widget state ─────────────────────────────────────────────────────────── */
static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct central_status_state {
    /* top panel */
    uint8_t  battery;
    bool     charging;
    bool     ble_connected;
    bool     ble_bonded;
    uint8_t  layer_index;
    const char *layer_label;
    /* bottom panel — last pressed key */
    char     key_str[8];
};

/* ─── drawing ───────────────────────────────────────────────────────────────── */

/*
 * Top panel: battery bar on left, layer name centred, BT icon top-right.
 * All drawing happens inside a 68x68 canvas that is then rotated 90°.
 */
static void draw_top(lv_obj_t *widget, lv_color_t cbuf[],
                     const struct central_status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_rect_dsc_t rect_bg, rect_fg, rect_bar;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    init_rect_dsc(&rect_fg, LVGL_FOREGROUND);
    init_rect_dsc(&rect_bar, LVGL_FOREGROUND);

    lv_draw_label_dsc_t lbl_layer, lbl_icon;
    init_label_dsc(&lbl_layer, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
    init_label_dsc(&lbl_icon,  LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);

    /* background */
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    /* ── battery bar (top strip, 68 px wide, 8 px tall) ── */
    /* outline */
    lv_canvas_draw_rect(canvas, 0, 1, 60, 10, &rect_fg);
    lv_canvas_draw_rect(canvas, 1, 2, 58, 8,  &rect_bg);
    /* fill: level is 0–100 */
    uint8_t fill = (uint8_t)((state->battery * 56u + 50u) / 100u);
    if (fill > 0) {
        lv_canvas_draw_rect(canvas, 2, 3, fill, 6, &rect_bar);
    }
    /* terminal nub */
    lv_canvas_draw_rect(canvas, 60, 3, 4, 6, &rect_fg);
    lv_canvas_draw_rect(canvas, 61, 4, 2, 4, &rect_bg);

    /* ── BT / USB icon top-right ── */
    const char *icon = state->ble_bonded
        ? (state->ble_connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE)
        : LV_SYMBOL_SETTINGS;
    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &lbl_icon, icon);

    /* ── layer name centred ── */
    char layer_text[16] = {};
    if (state->layer_label != NULL && strlen(state->layer_label) > 0) {
        strncpy(layer_text, state->layer_label, sizeof(layer_text) - 1);
    } else {
        snprintf(layer_text, sizeof(layer_text), "L%d", state->layer_index);
    }
    lv_canvas_draw_text(canvas, 0, 22, CANVAS_SIZE, &lbl_layer, layer_text);

    rotate_canvas(canvas, cbuf);
}

/*
 * Bottom panel: the last-pressed key glyph, drawn very large and centred.
 */
static void draw_bottom(lv_obj_t *widget, lv_color_t cbuf[],
                        const struct central_status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);

    lv_draw_label_dsc_t lbl_key;
    init_label_dsc(&lbl_key, LVGL_FOREGROUND, &lv_font_montserrat_26, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    /* centre vertically: font height ~26 px, canvas 68 px → top at y=21 */
    lv_canvas_draw_text(canvas, 0, 21, CANVAS_SIZE, &lbl_key, state->key_str);

    rotate_canvas(canvas, cbuf);
}

/* ─── widget instance ─────────────────────────────────────────────────────── */

static void redraw_all(struct zmk_widget_central_status *widget) {
    draw_top(widget->obj, widget->cbuf_top, &widget->state);
    draw_bottom(widget->obj, widget->cbuf_bot, &widget->state);
}

/* ─── battery listener ────────────────────────────────────────────────────── */

struct battery_status_state {
    uint8_t level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    bool usb_present;
#endif
};

static void battery_status_update_cb(struct battery_status_state st) {
    struct zmk_widget_central_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        w->state.battery = st.level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        w->state.charging = st.usb_present;
#endif
        draw_top(w->obj, w->cbuf_top, &w->state);
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

/* ─── output (BLE/USB) listener ──────────────────────────────────────────── */

struct output_status_state {
    bool ble_connected;
    bool ble_bonded;
};

static void output_status_update_cb(struct output_status_state st) {
    struct zmk_widget_central_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        w->state.ble_connected = st.ble_connected;
        w->state.ble_bonded    = st.ble_bonded;
        draw_top(w->obj, w->cbuf_top, &w->state);
    }
}

static struct output_status_state output_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .ble_connected = zmk_ble_active_profile_is_connected(),
        .ble_bonded    = !zmk_ble_active_profile_is_open(),
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

/* ─── layer listener ─────────────────────────────────────────────────────── */

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

static void layer_status_update_cb(struct layer_status_state st) {
    struct zmk_widget_central_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        w->state.layer_index = st.index;
        w->state.layer_label = st.label;
        draw_top(w->obj, w->cbuf_top, &w->state);
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

/* ─── key-press listener ─────────────────────────────────────────────────── */

struct key_press_state {
    uint16_t usage_page;
    uint32_t keycode;
    uint8_t  mods;      /* explicit_modifiers from the event */
    bool     pressed;
};

static void key_press_update_cb(struct key_press_state st) {
    if (!st.pressed) {
        return; /* only update on key-down */
    }

    struct zmk_widget_central_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        const char *s = keycode_to_str(st.usage_page, st.keycode, st.mods);
        strncpy(w->state.key_str, s, sizeof(w->state.key_str) - 1);
        w->state.key_str[sizeof(w->state.key_str) - 1] = '\0';
        draw_bottom(w->obj, w->cbuf_bot, &w->state);
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

/* ─── public init ────────────────────────────────────────────────────────── */

int zmk_widget_central_status_init(struct zmk_widget_central_status *widget,
                                    lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    /* Top canvas: right half of the rotated display (layer + battery) */
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf_top, CANVAS_SIZE, CANVAS_SIZE,
                         LV_IMG_CF_TRUE_COLOR);

    /* Bottom canvas: left half (big key glyph) */
    lv_obj_t *bot = lv_canvas_create(widget->obj);
    lv_obj_align(bot, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(bot, widget->cbuf_bot, CANVAS_SIZE, CANVAS_SIZE,
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
