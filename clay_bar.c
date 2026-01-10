/*
 * clay_bar.c
 *
 * Minimal example: Clay + Cairo + X11 status bar that:
 *  - shows date/time (top-left)
 *  - shows dwm status (root window name) in center
 *  - has a clickable left region which toggles a dropdown menu
 *
 * Requirements:
 *  - put clay.h next to this file (or update include path)
 *  - build: gcc -O2 -Wall clay_bar.c -o clay_bar -lX11 -lcairo -lm
 *
 * This file defines CLAY_IMPLEMENTATION once (so include clay.h below).
 */

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <math.h>

/* ------------------------------------------------------------------
   Include Clay single-header implementation. Put clay.h in the same
   directory or adjust the path (e.g. third_party/clay.h).
   ------------------------------------------------------------------ */
#define CLAY_IMPLEMENTATION
#include "clay.h"

/* ------------------------------------------------------------------
   Small helper: spawn command (fork/execvp). Returns child pid or -1.
   ------------------------------------------------------------------ */
static pid_t spawn_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    } else if (pid > 0) {
        return pid;
    } else {
        perror("fork");
        return -1;
    }
}

/* ------------------------------------------------------------------
   Constants and Configuration
   ------------------------------------------------------------------ */
static const int BAR_HEIGHT = 26;
static const int REFRESH_INTERVAL_MS = 500;  /* 0.5 seconds */
static const int SLEEP_INTERVAL_US = 10000; /* 10ms */
static const char *DEFAULT_TERMINAL[] = { "xterm", NULL };
static const char *DEFAULT_FILE_MANAGER[] = { "xdg-open", ".", NULL };

/* ------------------------------------------------------------------
   Global state for the bar
   ------------------------------------------------------------------ */
static Display *dpy = NULL;
static int screen_num = 0;
static Window rootwin;
static Window barwin;
static int screen_w = 0, screen_h = 0;

static cairo_surface_t *surf = NULL;
static cairo_t *cr = NULL;
/* Pango objects (used for measuring and optional global context) */
static PangoFontMap *pango_fontmap = NULL;
static PangoContext *pango_context = NULL;

/* Clay arena memory */
static void *clay_mem = NULL;
static Clay_Arena clay_arena;

/* dropdown state */
static int dropdown_open = 0;

/* text to display */
static char timebuf[64] = {0};
static char statusbuf[512] = {0};


static Clay_Dimensions measure_text_fn(Clay_StringSlice s, Clay_TextElementConfig *cfg, uintptr_t userData) {
    /* userData will contain a PangoContext* (passed during registration) */
    PangoContext *ctx = (PangoContext *) (uintptr_t) userData;
    if (!ctx) {
        /* fallback conservative estimate */
        float fs = cfg ? (float)cfg->fontSize : 12.0f;
        return (Clay_Dimensions){ .width = (float)s.length * (fs * 0.55f), .height = fs };
    }

    /* create a temporary PangoLayout using the provided context */
    PangoLayout *layout = pango_layout_new(ctx);
    if (!layout) {
        float fs = cfg ? (float)cfg->fontSize : 12.0f;
        return (Clay_Dimensions){ .width = (float)s.length * (fs * 0.55f), .height = fs };
    }

    /* convert Clay_StringSlice to NUL-terminated UTF-8 */
    size_t n = (size_t)s.length;
    char *tmp = malloc(n + 1);
    if (!tmp) {
        g_object_unref(layout);
        float fs = cfg ? (float)cfg->fontSize : 12.0f;
        return (Clay_Dimensions){ .width = (float)s.length * (fs * 0.55f), .height = fs };
    }
    memcpy(tmp, s.chars, n);
    tmp[n] = '\0';

    /* Build a font description from cfg->fontSize (and optional family if you add it) */
    PangoFontDescription *desc = pango_font_description_new();
    /* optionally set family:
       pango_font_description_set_family(desc, "Sans");
    */
    int pango_size = (int)((cfg && cfg->fontSize > 0.0f) ? (cfg->fontSize * PANGO_SCALE) : (12 * PANGO_SCALE));
    pango_font_description_set_size(desc, pango_size);
    pango_layout_set_font_description(layout, desc);

    /* set text and measure */
    pango_layout_set_text(layout, tmp, -1);
    int width_px = 0, height_px = 0;
    pango_layout_get_pixel_size(layout, &width_px, &height_px);

    /* cleanup */
    pango_font_description_free(desc);
    free(tmp);
    g_object_unref(layout);

    return (Clay_Dimensions){ .width = (float)width_px, .height = (float)height_px };
}

/* error handler for Clay */
static void clay_error_handler(Clay_ErrorData err) {
    /* print to stderr */
    if (err.errorText.chars && err.errorText.length > 0) {
        /* ensure NUL terminated safely in small buffer */
        size_t n = (size_t)err.errorText.length;
        char *tmp = malloc(n + 1);
        if (tmp) {
            memcpy(tmp, err.errorText.chars, n);
            tmp[n] = '\0';
            fprintf(stderr, "Clay error: %s\n", tmp);
            free(tmp);
        }
    }
}

/* initialize Clay memory + context */
static void init_clay(int width, int height) {
    /* determine minimum memory required by Clay */
    size_t min_mem = Clay_MinMemorySize();
    clay_mem = malloc(min_mem);
    if (!clay_mem) {
        fprintf(stderr, "failed to allocate clay memory\n");
        exit(1);
    }
    clay_arena = Clay_CreateArenaWithCapacityAndMemory(min_mem, clay_mem);

    Clay_Initialize(clay_arena, (Clay_Dimensions){ .width = (float)width, .height = (float)BAR_HEIGHT },
                    (Clay_ErrorHandler)clay_error_handler);

/* create a Pango context for measurement (fontmap/context lifetime managed by us) */
pango_fontmap = pango_cairo_font_map_get_default();
if (pango_fontmap) {
    /* create a PangoContext from the fontmap */
    pango_context = pango_font_map_create_context(pango_fontmap);
} else {
    pango_context = NULL;
}

/* register the measure function with Clay, passing pango_context as userData */
Clay_SetMeasureTextFunction((Clay_MeasureTextFunction)measure_text_fn, (uintptr_t)pango_context);
}

/* helper to fetch root window name (dwm status typically updated there) */
static void update_root_status(void) {
    XTextProperty tprop;
    if (XGetWMName(dpy, rootwin, &tprop) && tprop.value) {
        size_t len = tprop.nitems;
        if (len > sizeof(statusbuf)-1) len = sizeof(statusbuf)-1;
        memcpy(statusbuf, tprop.value, len);
        statusbuf[len] = '\0';
        if (tprop.encoding) XFree(tprop.encoding);
    } else {
        statusbuf[0] = '\0';
    }
}

/* update time string (local time) */
static void update_time(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
}

/* Render Clay commands using Cairo - minimal renderer:
   - rects (background)
   - text
   This renderer ignores many Clay features, but is enough for the demo.
*/
static void render_clay_with_cairo(void) {
    /* Update clay layout dims (if window width changed) */
    Clay_SetLayoutDimensions((Clay_Dimensions){ .width = (float)screen_w, .height = (float)BAR_HEIGHT });

    /* Inform Clay of pointer state = not using pointer here (we only handle clicks via X events) */
    Clay_SetPointerState((Clay_Vector2){0,0}, CLAY_POINTER_DATA_RELEASED);

    /* Build layout */
    Clay_BeginLayout();

    /* Outer root container (full width, fixed height) */
    CLAY({
        .id = CLAY_ID("root"),
        .backgroundColor = { 18, 18, 18, 255 },
        .layout = CLAY__CONFIG_WRAPPER(Clay_LayoutConfig, {
            .sizing = CLAY__CONFIG_WRAPPER(Clay_Sizing, {
                .width = CLAY_SIZING_FIXED((float)screen_w),
                .height = CLAY_SIZING_FIXED((float)BAR_HEIGHT)
            })
        })
    }) {
        /* Left area: time + clickable icon */
        CLAY({
            .id = CLAY_ID("left"),
            .layout = CLAY__CONFIG_WRAPPER(Clay_LayoutConfig, {
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .sizing = CLAY__CONFIG_WRAPPER(Clay_Sizing, {
                    .width = CLAY_SIZING_FIXED(250),
                    .height = CLAY_SIZING_FIXED((float)BAR_HEIGHT)
                }),
                .childGap = 6,
                .padding = CLAY_PADDING_ALL(6)
            }),
            .backgroundColor = { 25, 25, 25, 255 }
        }) {
            /* clickable icon / label - show truncated "Menu" */
            CLAY({
                .id = CLAY_ID("menu_toggle"),
                .layout = CLAY__CONFIG_WRAPPER(Clay_LayoutConfig, {
                    .sizing = CLAY__CONFIG_WRAPPER(Clay_Sizing, {
                        .width = CLAY_SIZING_FIXED(60),
                        .height = CLAY_SIZING_FIXED((float)BAR_HEIGHT - 12)
                    })
                }),
                .backgroundColor = { 40, 40, 40, 255 },
                .border = CLAY__CONFIG_WRAPPER(Clay_BorderElementConfig, {
                    .color = { 120, 120, 120, 255 },
                    .width = CLAY_BORDER_ALL(1)
                })
            }) {
                CLAY_TEXT(CLAY_STRING("Menu"), CLAY_TEXT_CONFIG({
                    .fontSize = 12,
                    .textColor = { 230, 230, 230, 255 },
                    .letterSpacing = 0
                }));
            }

            /* time text */
            CLAY({
                .id = CLAY_ID("time_text"),
                .layout = CLAY__CONFIG_WRAPPER(Clay_LayoutConfig, {
                    .sizing = CLAY__CONFIG_WRAPPER(Clay_Sizing, {
                        .width = CLAY_SIZING_FIXED(170),
                        .height = CLAY_SIZING_FIXED((float)BAR_HEIGHT - 12)
                    })
                })
            }) {
                CLAY_TEXT(CLAY_STRING((const char *)timebuf), CLAY_TEXT_CONFIG({
                    .fontSize = 12,
                    .textColor = { 220, 220, 220, 255 }
                }));
            }
        }

        /* Center area: show status from root window */
        CLAY({
            .id = CLAY_ID("center"),
            .layout = CLAY__CONFIG_WRAPPER(Clay_LayoutConfig, {
                .sizing = CLAY__CONFIG_WRAPPER(Clay_Sizing, {
                    .width = CLAY_SIZING_PERCENT(1.0f), /* take remaining width */
                    .height = CLAY_SIZING_FIXED((float)BAR_HEIGHT)
                }),
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            })
        }) {
            CLAY_TEXT(CLAY_STRING((const char *)statusbuf), CLAY_TEXT_CONFIG({
                .fontSize = 12,
                .textColor = { 200, 200, 200, 255 }
            }));
        }

        /* If dropdown is open, create a floating element attached to root */
        if (dropdown_open) {
            CLAY({
                .id = CLAY_ID("dropdown"),
                .floating = CLAY__CONFIG_WRAPPER(Clay_FloatingElementConfig, {
                    .attachTo = CLAY_ATTACH_TO_ROOT,
                    .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP },
                    .offset = { .x = 6.0f, .y = (float)BAR_HEIGHT }
                }),
                .zIndex = 100,
                .backgroundColor = { 28, 28, 28, 255 },
                .layout = CLAY__CONFIG_WRAPPER(Clay_LayoutConfig, {
                    .sizing = CLAY__CONFIG_WRAPPER(Clay_Sizing, {
                        .width = CLAY_SIZING_FIXED(180),
                        .height = CLAY_SIZING_FIXED(120)
                    }),
                    .padding = CLAY_PADDING_ALL(6),
                    .childGap = 6,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                }),
                .border = CLAY__CONFIG_WRAPPER(Clay_BorderElementConfig, {
                    .color = { 90, 90, 90, 255 },
                    .width = CLAY_BORDER_ALL(1)
                })
            }) {
                CLAY({
                    .id = CLAY_ID("item1"),
                    .layout = CLAY__CONFIG_WRAPPER(Clay_LayoutConfig, {
                        .sizing = CLAY__CONFIG_WRAPPER(Clay_Sizing, {
                            .width = CLAY_SIZING_GROW(0),
                            .height = CLAY_SIZING_FIXED(28)
                        })
                    })
                }) {
                    CLAY_TEXT(CLAY_STRING("Open xterm"), CLAY_TEXT_CONFIG({
                        .fontSize = 12, .textColor = { 220, 220, 220, 255 }
                    }));
                }

                CLAY({
                    .id = CLAY_ID("item2"),
                    .layout = CLAY__CONFIG_WRAPPER(Clay_LayoutConfig, {
                        .sizing = CLAY__CONFIG_WRAPPER(Clay_Sizing, {
                            .width = CLAY_SIZING_GROW(0),
                            .height = CLAY_SIZING_FIXED(28)
                        })
                    })
                }) {
                    CLAY_TEXT(CLAY_STRING("Show files (xdg-open .)"), CLAY_TEXT_CONFIG({
                        .fontSize = 12, .textColor = { 220, 220, 220, 255 }
                    }));
                }
            }
        }
    } /* end CLAY root */

    Clay_EndLayout();

    /* get render commands and draw them */
    Clay_RenderCommandArray cmds = Clay_GetRenderCommands();

    /* clear with transparent background first (we draw root background via clay) */
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* iterate commands */
    for (int i = 0; i < cmds.length; ++i) {
        Clay_RenderCommand *cmd = &cmds.internalArray[i];
        Clay_BoundingBox bb = cmd->boundingBox;
        float x = bb.x;
        float y = bb.y;
        float w = bb.width;
        float h = bb.height;

        switch (cmd->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            Clay_RectangleRenderData *rd = &cmd->renderData.rectangle;
            cairo_rectangle(cr, x, y, w, h);
            cairo_set_source_rgba(cr, rd->backgroundColor.r / 255.0, rd->backgroundColor.g / 255.0, rd->backgroundColor.b / 255.0, rd->backgroundColor.a / 255.0);
            cairo_fill(cr);
        } break;
        case CLAY_RENDER_COMMAND_TYPE_BORDER: {
            Clay_BorderRenderData *br = &cmd->renderData.border;
            /* draw a simple rectangle border */
            cairo_set_line_width(cr, (double)br->width.top);
            cairo_rectangle(cr, x + br->width.left/2.0, y + br->width.top/2.0, w - (br->width.left + br->width.right)/2.0, h - (br->width.top + br->width.bottom)/2.0);
            cairo_set_source_rgba(cr, br->color.r / 255.0, br->color.g / 255.0, br->color.b / 255.0, br->color.a / 255.0);
            cairo_stroke(cr);
        } break;
        case CLAY_RENDER_COMMAND_TYPE_TEXT: {
            Clay_TextRenderData *tr = &cmd->renderData.text;

            /* create a PangoLayout tied to this cairo context */
            PangoLayout *layout = pango_cairo_create_layout(cr);
            if (!layout) break;

             /* convert Clay_StringSlice to NUL-terminated UTF-8 */
            size_t n = (size_t)tr->stringContents.length;
            char *tmp = malloc(n + 1);
            if (!tmp) { g_object_unref(layout); break; }
            memcpy(tmp, tr->stringContents.chars, n);
            tmp[n] = '\0';

            /* set text on layout */
            pango_layout_set_text(layout, tmp, -1);

            /* set font description from tr->fontSize */
            PangoFontDescription *desc = pango_font_description_new();
            int pango_size = (int)( (tr->fontSize > 0.0f ? tr->fontSize : 12.0f) * PANGO_SCALE );
            pango_font_description_set_size(desc, pango_size);
            /* optional: set family: pango_font_description_set_family(desc, "Sans"); */
            pango_layout_set_font_description(layout, desc);

            /* compute vertical alignment: Pango returns pixel extents */
            int pw = 0, ph = 0;
            pango_layout_get_pixel_size(layout, &pw, &ph);

            double text_x = x + 2.0; /* small left padding inside element */
            double text_y = y + ((double)h - (double)ph) / 2.0; /* center vertically */

            /* set color */
            cairo_set_source_rgba(cr, tr->textColor.r/255.0, tr->textColor.g/255.0, tr->textColor.b/255.0, tr->textColor.a/255.0);

            /* draw */
            cairo_move_to(cr, text_x, text_y);
            pango_cairo_show_layout(cr, layout);

            /* cleanup */
            pango_font_description_free(desc);
            free(tmp);
            g_object_unref(layout);
        } break;

        default:
            /* ignore other types for now */
            break;
        }
    }

    cairo_surface_flush(surf);
    XFlush(dpy);
}

/* helper: test if point inside a Clay element's bounding box */
static int point_in_element(const char *id_str, int px, int py) {
    Clay_ElementData ed = Clay_GetElementData(CLAY_ID(id_str));
    if (!ed.found) return 0;
    float x = ed.boundingBox.x;
    float y = ed.boundingBox.y;
    float w = ed.boundingBox.width;
    float h = ed.boundingBox.height;
    return (px >= (int)x && px < (int)(x + w) && py >= (int)y && py < (int)(y + h));
}

/* main loop */
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* handle SIGCHLD to avoid zombies from spawn */
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

    /* Create override-redirect window at top for our bar */
    XSetWindowAttributes at;
    at.override_redirect = True;
    at.background_pixmap = None;
    at.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask;

    barwin = XCreateWindow(dpy, rootwin, 0, 0, screen_w, BAR_HEIGHT,
                           0, CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWEventMask | CWBackPixmap, &at);

    XMapWindow(dpy, barwin);
    XRaiseWindow(dpy, barwin);

    /* create cairo surface for that window */
    Visual *vis = DefaultVisual(dpy, screen_num);
    surf = cairo_xlib_surface_create(dpy, barwin, vis, screen_w, BAR_HEIGHT);
    cr = cairo_create(surf);

    /* init Clay */
    init_clay(screen_w, BAR_HEIGHT);

    /* initial update */
    update_time();
    update_root_status();
    render_clay_with_cairo();

    /* event + (very simple) refresh loop */
    XEvent ev;
    struct timespec last_time = {0};
    clock_gettime(CLOCK_MONOTONIC, &last_time);

    for (;;) {
        /* poll events with small timeout using XPending/XNextEvent */
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            if (ev.type == Expose) {
                render_clay_with_cairo();
            } else if (ev.type == ConfigureNotify) {
                XConfigureEvent *ce = &ev.xconfigure;
                if (ce->width != screen_w && ce->width > 0) {
                    screen_w = ce->width;
                    /* resize cairo surface */
                    cairo_xlib_surface_set_size(surf, screen_w, BAR_HEIGHT);
                    /* update Clay layout dims */
                    Clay_SetLayoutDimensions((Clay_Dimensions){ .width = (float)screen_w, .height = (float)BAR_HEIGHT });
                    render_clay_with_cairo();
                }
            } else if (ev.type == ButtonPress) {
                XButtonEvent *b = &ev.xbutton;
                /* validate button event */
                if (b->button == Button1) { /* Only handle left mouse button */
                    /* translate y coordinates relative to bar top */
                    int mx = b->x;
                    int my = b->y;
                    /* if clicked in our menu toggle region, toggle dropdown */
                    if (point_in_element("menu_toggle", mx, my)) {
                        dropdown_open = !dropdown_open;
                        render_clay_with_cairo();
                    } else if (dropdown_open) {
                        /* test if clicked item1 or item2 */
                        if (point_in_element("item1", mx, my)) {
                            /* spawn xterm */
                            char *term[] = { "xterm", NULL };
                            spawn_cmd(term);
                            dropdown_open = 0;
                            render_clay_with_cairo();
                        } else if (point_in_element("item2", mx, my)) {
                            /* spawn xdg-open . */
                            char *args[] = { "xdg-open", ".", NULL };
                            spawn_cmd(args);
                            dropdown_open = 0;
                            render_clay_with_cairo();
                        } else {
                            /* click elsewhere closes dropdown */
                            dropdown_open = 0;
                            render_clay_with_cairo();
                        }
                    } else {
                        /* click outside -- ignore */
                    }
                }
            } else if (ev.type == ButtonRelease) {
                /* ignore */
            }
        }

        /* periodic updates (every 0.5s) */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - last_time.tv_sec) + (now.tv_nsec - last_time.tv_nsec) / 1e9;
        if (elapsed > 0.5) {
            last_time = now;
            update_time();
            update_root_status();
            render_clay_with_cairo();
        }

        /* tiny sleep to avoid busy loop */
        usleep(10000);
    }

    /* cleanup (never reached) */
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    XDestroyWindow(dpy, barwin);
    XCloseDisplay(dpy);
    free(clay_mem);
    if (pango_context) g_object_unref(pango_context);
    if (pango_fontmap) g_object_unref(pango_fontmap);
    pango_context = NULL;
    pango_fontmap = NULL;
    return 0;
}
