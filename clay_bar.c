/*
 * clay_bar.c
 *
 * Shaped top-right time display with no background.
 *
 * Compile:
 *   gcc -O2 -Wall -Wextra -std=c99 -o clay_bar clay_bar.c -lX11 -lcairo -lm
 *
 * Notes:
 * - Only uses XShape to make the window match text region.
 * - No background rectangle. Only text is visible.
 */

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

/* ---------- CONFIG ---------- */
static const char *FONT_FACE = "monospace";
static const double FONT_SIZE = 18.0;
static const int H_PADDING = 7;
static const int V_PADDING = 3;
static const int FG_R = 220, FG_G = 220, FG_B = 220;
static const double FG_A = 1.0;
static const double REFRESH_INTERVAL = 60.0;
/* ---------------------------- */

static Display *dpy = NULL;
static int screen_num = 0;
static Window rootwin = 0;
static Window barwin = 0;
static int screen_w = 0, screen_h = 0;

static cairo_surface_t *surf = NULL;
static cairo_t *cr = NULL;

static char timebuf[128] = {0};

static int shape_available = 0;
static Pixmap shape_pixmap = 0;

/* Update time string */
static void update_time(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(timebuf, sizeof(timebuf), "%a/%-d %I:%M%p", &tm);
}

/* Ensure Cairo surface matches window size */
static void ensure_surface_size(int w, int h) {
    if (!surf) {
        surf = cairo_xlib_surface_create(dpy, barwin, DefaultVisual(dpy, screen_num), w, h);
        cr = cairo_create(surf);
    } else {
        cairo_xlib_surface_set_size(surf, w, h);
    }
}

/* Create 1-bit mask pixmap from Cairo surface */
static Pixmap create_mask_from_a8(cairo_surface_t *mask_surf, int w, int h) {
    if (!mask_surf || w <= 0 || h <= 0) return 0;
    unsigned char *data = cairo_image_surface_get_data(mask_surf);
    int stride = cairo_image_surface_get_stride(mask_surf);
    int bytes_per_row = (w + 7) / 8;
    unsigned char *bitmap = calloc(bytes_per_row * h, 1);
    if (!bitmap) return 0;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char a = data[y*stride + x];
            if (a > 0) {
                int byte_index = y * bytes_per_row + (x / 8);
                int bit = x % 8;
                bitmap[byte_index] |= (1 << bit);
            }
        }
    }

    Pixmap pm = XCreateBitmapFromData(dpy, barwin, (char *)bitmap, w, h);
    free(bitmap);
    return pm;
}

/* Update shaped window mask for text only */
static void update_shape_mask(int win_w, int win_h, const char *text) {
    if (!shape_available) return;

    cairo_surface_t *mask_surf = cairo_image_surface_create(CAIRO_FORMAT_A8, win_w, win_h);
    cairo_t *mask_cr = cairo_create(mask_surf);

    /* clear */
    cairo_set_operator(mask_cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(mask_cr);
    cairo_set_operator(mask_cr, CAIRO_OPERATOR_OVER);

    /* draw text into mask */
    cairo_select_font_face(mask_cr, FONT_FACE, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(mask_cr, FONT_SIZE);
    cairo_font_extents_t fe;
    cairo_font_extents(mask_cr, &fe);
    cairo_text_extents_t te;
    cairo_text_extents(mask_cr, text, &te);
    double x = H_PADDING - te.x_bearing;
    double y = (win_h - fe.height) / 2.0 + fe.ascent;
    cairo_set_source_rgba(mask_cr, 1.0, 1.0, 1.0, 1.0);
    cairo_move_to(mask_cr, x, y);
    cairo_show_text(mask_cr, text);

    cairo_surface_flush(mask_surf);

    Pixmap new_mask = create_mask_from_a8(mask_surf, win_w, win_h);
    if (new_mask) {
        if (shape_pixmap) XFreePixmap(dpy, shape_pixmap);
        shape_pixmap = new_mask;
        XShapeCombineMask(dpy, barwin, ShapeBounding, 0, 0, shape_pixmap, ShapeSet);
    }

    cairo_destroy(mask_cr);
    cairo_surface_destroy(mask_surf);
}

/* Render text only */
static void render_now(void) {
    if (!cr || !surf || !barwin) return;

    cairo_select_font_face(cr, FONT_FACE, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, FONT_SIZE);
    cairo_text_extents_t te;
    cairo_text_extents(cr, timebuf, &te);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);

    int text_w = (int)(te.width + 0.5);
    int win_w = text_w + 2 * H_PADDING;
    int win_h = (int)(fe.height + 0.5) + 2 * V_PADDING;
    if (win_h < 1) win_h = 1;

    XWindowAttributes wattr;
    XGetWindowAttributes(dpy, barwin, &wattr);
    if ((int)wattr.width != win_w || (int)wattr.height != win_h) {
        XMoveResizeWindow(dpy, barwin, screen_w - win_w - H_PADDING, 0, win_w, win_h);
        ensure_surface_size(win_w, win_h);
    }

    update_shape_mask(win_w, win_h, timebuf);

    /* clear surface */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    double y = (win_h - fe.height) / 2.0 + fe.ascent;
    double x = H_PADDING - te.x_bearing;
    cairo_set_source_rgba(cr, FG_R/255.0, FG_G/255.0, FG_B/255.0, FG_A);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, timebuf);

    cairo_surface_flush(surf);
    XFlush(dpy);
}

/* cleanup */
static void cleanup(void) {
    if (shape_pixmap) XFreePixmap(dpy, shape_pixmap);
    if (cr) cairo_destroy(cr);
    if (surf) cairo_surface_destroy(surf);
    if (barwin) XDestroyWindow(dpy, barwin);
    if (dpy) XCloseDisplay(dpy);
}

/* main */
int main(void) {
    /* ignore SIGCHLD */
    struct sigaction sa = {0};
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "clay_bar: failed to open display\n");
        return 1;
    }

    int shape_event_base, shape_error_base;
    shape_available = XShapeQueryExtension(dpy, &shape_event_base, &shape_error_base);

    screen_num = DefaultScreen(dpy);
    rootwin = RootWindow(dpy, screen_num);
    screen_w = DisplayWidth(dpy, screen_num);
    screen_h = DisplayHeight(dpy, screen_num);

    /* create simple override-redirect window */
    XSetWindowAttributes at;
    at.override_redirect = True;
    at.background_pixmap = None;
    at.event_mask = ExposureMask | StructureNotifyMask;
    barwin = XCreateWindow(dpy, rootwin, 0, 0, 200, 50, 0,
                            DefaultDepth(dpy, screen_num),
                            InputOutput, DefaultVisual(dpy, screen_num),
                            CWOverrideRedirect | CWBackPixmap | CWEventMask, &at);
    XMapWindow(dpy, barwin);
    XRaiseWindow(dpy, barwin);

    /* initial time and surface */
    update_time();
    ensure_surface_size(200, 50);
    render_now();

    /* main loop */
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    XEvent ev;

    for (;;) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            if (ev.type == Expose) render_now();
            else if (ev.type == ConfigureNotify) {
                XConfigureEvent *ce = &ev.xconfigure;
                if (ce->width > 0 && ce->height > 0) {
                    ensure_surface_size(ce->width, ce->height);
                    render_now();
                }
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
        if (elapsed >= REFRESH_INTERVAL) {
            last = now;
            update_time();
            render_now();
        }

        usleep(10000);
    }

    cleanup();
    return 0;
}
