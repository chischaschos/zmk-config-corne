/*
 * Copyright (c) 2024 ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include "util.h"

/*
 * rotate_canvas -- square 68x68 rotation (used by peripheral).
 */
void rotate_canvas(lv_obj_t *canvas, lv_color_t cbuf[]) {
    static lv_color_t cbuf_tmp[CANVAS_SIZE * CANVAS_SIZE];
    memcpy(cbuf_tmp, cbuf, sizeof(cbuf_tmp));

    lv_img_dsc_t img;
    img.data = (void *)cbuf_tmp;
    img.header.cf = LV_IMG_CF_TRUE_COLOR;
    img.header.w = CANVAS_SIZE;
    img.header.h = CANVAS_SIZE;

    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);
    lv_canvas_transform(canvas, &img, 900, LV_IMG_ZOOM_NONE, -1, 0, CANVAS_SIZE / 2,
                        CANVAS_SIZE / 2, true);
}

/*
 * rotate_canvas_rect -- rectangular rotation for central tiles.
 *
 * The canvas is set up as src_w x src_h (e.g. 68x40) for drawing.
 * After drawing, we manually rotate the pixel data 90 deg CW and
 * reconfigure the canvas to src_h x src_w (e.g. 40x68) so LVGL
 * renders the rotated result.
 *
 * 90 deg CW rotation:
 *   dst(dy, dx) = src(sy, sx)
 *   dx = sy
 *   dy = (src_w - 1) - sx
 */
void rotate_canvas_rect(lv_obj_t *canvas, lv_color_t cbuf[],
                        uint16_t src_w, uint16_t src_h) {
    uint16_t dst_w = src_h;
    uint16_t dst_h = src_w;

    /* temp copy since cbuf is the canvas backing buffer */
    static lv_color_t tmp[CANVAS_W * CANVAS_W]; /* 68*68 = big enough */
    uint32_t src_size = (uint32_t)src_w * src_h;
    memcpy(tmp, cbuf, src_size * sizeof(lv_color_t));

    /* clear */
    for (uint32_t i = 0; i < (uint32_t)(dst_w * dst_h); i++) {
        cbuf[i] = LVGL_BACKGROUND;
    }

    /* rotate 90 deg CCW (matches LVGL's 900 deg transform used by peripheral)
     * CCW: dx = (src_h - 1) - sy,  dy = sx
     */
    for (uint16_t sy = 0; sy < src_h; sy++) {
        for (uint16_t sx = 0; sx < src_w; sx++) {
            uint16_t dx = (src_h - 1) - sy;
            uint16_t dy = sx;
            cbuf[dy * dst_w + dx] = tmp[sy * src_w + sx];
        }
    }

    /* reconfigure canvas to rotated dimensions */
    lv_canvas_set_buffer(canvas, cbuf, dst_w, dst_h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(canvas);
}

void init_label_dsc(lv_draw_label_dsc_t *label_dsc, lv_color_t color, const lv_font_t *font,
                    lv_text_align_t align) {
    lv_draw_label_dsc_init(label_dsc);
    label_dsc->color = color;
    label_dsc->font = font;
    label_dsc->align = align;
}

void init_rect_dsc(lv_draw_rect_dsc_t *rect_dsc, lv_color_t bg_color) {
    lv_draw_rect_dsc_init(rect_dsc);
    rect_dsc->bg_color = bg_color;
}
