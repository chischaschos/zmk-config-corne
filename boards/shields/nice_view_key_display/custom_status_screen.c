/*
 * Copyright (c) 2024 ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * custom_status_screen.c
 *
 * ZMK calls zmk_display_status_screen() once at boot.  We return an lv_obj_t*
 * that contains our widget tree.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>

#if IS_ENABLED(CONFIG_NICE_VIEW_KEY_DISPLAY_WIDGET)

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include "widgets/central_status.h"
static struct zmk_widget_central_status status_widget;
#else
#include "widgets/peripheral_status.h"
static struct zmk_widget_peripheral_status status_widget;
#endif

#endif /* CONFIG_NICE_VIEW_KEY_DISPLAY_WIDGET */

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

#if IS_ENABLED(CONFIG_NICE_VIEW_KEY_DISPLAY_WIDGET)

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zmk_widget_central_status_init(&status_widget, screen);
    lv_obj_align(zmk_widget_central_status_obj(&status_widget), LV_ALIGN_TOP_LEFT, 0, 0);
#else
    zmk_widget_peripheral_status_init(&status_widget, screen);
    lv_obj_align(zmk_widget_peripheral_status_obj(&status_widget), LV_ALIGN_TOP_LEFT, 0, 0);
#endif

#endif /* CONFIG_NICE_VIEW_KEY_DISPLAY_WIDGET */

    return screen;
}
