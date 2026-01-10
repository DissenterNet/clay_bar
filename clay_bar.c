/*
 * clay_bar.c
 *
 * Minimal working status bar using X11 + Cairo
 */

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define _GNU_SOURCE
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <math.h>

static const int BAR_HEIGHT = 26;

/* Global state */
static Display *dpy = NULL;
static int screen_num = 0;
static Window rootwin;
static Window barwin;
static int screen_w = 0, screen_h = 0;

static cairo_surface_t *surf = NULL;
static cairo_t *cr = NULL;


/* text to display */
static char timebuf[64] = {0};
static char statusbuf[512] = {0};


/* helper: draw text with Cairo (simple) */
static void draw_text(int x, int y, const char *text, int r, int g, int b) {
    if (!cr) return;
    
    cairo_set_source_rgb(cr, r/255.0, g/255.0, b/255.0);
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 18.0);  /* 25% bigger: 14.0 * 1.25 = 17.5, rounded to 18 */
    cairo_move_to(cr, x, y + BAR_HEIGHT/2 + 1);  /* Center vertically */
    cairo_show_text(cr, text);
}


/* helper: fetch root window name - simplified */
static void update_root_status(void) {
    statusbuf[0] = '\0';  /* Clear status buffer */
}

/* update time string */
static void update_time(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
}

/* render the bar */
static void render_bar(void) {
    /* time on right side with proper positioning */
    cairo_text_extents_t time_extents;
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 18.0);
    cairo_text_extents(cr, timebuf, &time_extents);
    int time_x = screen_w - time_extents.width - 20;
    draw_text(time_x, 0, timebuf, 220, 220, 220);
    
    /* center status text */
    if (strlen(statusbuf) > 0) {
        cairo_text_extents_t status_extents;
        cairo_text_extents(cr, statusbuf, &status_extents);
        int status_x = screen_w/2 - status_extents.width/2;
        draw_text(status_x, 0, statusbuf, 200, 200, 200);
    }
    
    cairo_surface_flush(surf);
    XFlush(dpy);
}

/* main loop */
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    /* handle SIGCHLD */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }
    
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "failed to open display\n");
        return 1;
    }
    screen_num = DefaultScreen(dpy);
    rootwin = RootWindow(dpy, screen_num);
    screen_w = DisplayWidth(dpy, screen_num);
    screen_h = DisplayHeight(dpy, screen_num);
    
    /* Create window */
    XSetWindowAttributes at;
    at.override_redirect = True;
    at.background_pixmap = None;
    at.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask;
    
    barwin = XCreateWindow(dpy, rootwin, 0, 0, screen_w, BAR_HEIGHT,
                           0, CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWEventMask | CWBackPixmap, &at);
    
    /* Force transparency immediately after creation */
    XSetWindowBackground(dpy, barwin, 0);
    XClearWindow(dpy, barwin);
    
    XMapWindow(dpy, barwin);
    XRaiseWindow(dpy, barwin);
    
    /* create cairo surface */
    Visual *vis = DefaultVisual(dpy, screen_num);
    surf = cairo_xlib_surface_create(dpy, barwin, vis, screen_w, BAR_HEIGHT);
    cr = cairo_create(surf);
    
    /* initial update */
    update_time();
    update_root_status();
    render_bar();
    
    /* event loop */
    XEvent ev;
    struct timespec last_time = {0};
    clock_gettime(CLOCK_MONOTONIC, &last_time);
    
    for (;;) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            if (ev.type == Expose) {
                render_bar();
            } else if (ev.type == ConfigureNotify) {
                XConfigureEvent *ce = &ev.xconfigure;
                if (ce->width != screen_w && ce->width > 0) {
                    screen_w = ce->width;
                    cairo_xlib_surface_set_size(surf, screen_w, BAR_HEIGHT);
                    render_bar();
                }
            }
        }
        
        /* periodic updates */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - last_time.tv_sec) + (now.tv_nsec - last_time.tv_nsec) / 1e9;
        if (elapsed > 0.5) {
            last_time = now;
            update_time();
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - last_time.tv_sec) + (now.tv_nsec - last_time.tv_nsec) / 1e9;
        if (elapsed > 0.5) {
            last_time = now;
            update_time();
            update_root_status();
            render_bar();
        }
        
        sleep(1);
    }
    
    /* cleanup */
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    XDestroyWindow(dpy, barwin);
    XCloseDisplay(dpy);
    return 0;
}
