#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "qemu-common.h"
#include "console.h"
#include "sysemu.h"
#include "qmp-commands.h"
#include "x_keymap.h"
#include "keymaps.h"

typedef struct GtkDisplayState
{
    GtkWidget *window;
    GtkWidget *drawing_area;
    cairo_surface_t *surface;
    DisplayChangeListener dcl;
    DisplayState *ds;
    int button_mask;
    int last_x;
    int last_y;
} GtkDisplayState;

static GtkDisplayState *global_state;

#define DEBUG_GTK

#ifdef DEBUG_GTK
#define dprintf(fmt, ...) printf(fmt, ## __VA_ARGS__)
#else
#define dprintf(fmt, ...) do { } while (0)
#endif

static gboolean gd_window_close(GtkWidget *widget, GdkEvent *event,
                                void *opaque)
{
    if (!no_quit) {
        qmp_quit(NULL);
        return FALSE;
    }

    return TRUE;
}

static void gd_update(DisplayState *ds, int x, int y, int w, int h)
{
    GtkDisplayState *s = ds->opaque;

//    dprintf("update(x=%d, y=%d, w=%d, h=%d)\n", x, y, w, h);

    gtk_widget_queue_draw_area(s->drawing_area, x, y, w, h);
}

static void gd_refresh(DisplayState *ds)
{
    vga_hw_update();
}

static void gd_resize(DisplayState *ds)
{
    GtkDisplayState *s = ds->opaque;
    cairo_format_t kind;
    int stride;

    dprintf("resize(width=%d, height=%d)\n",
            ds->surface->width, ds->surface->height);

    if (s->surface) {
        cairo_surface_destroy(s->surface);
    }

    switch (ds->surface->pf.bits_per_pixel) {
    case 8:
        kind = CAIRO_FORMAT_A8;
        break;
    case 16:
        kind = CAIRO_FORMAT_RGB16_565;
        break;
    case 32:
        kind = CAIRO_FORMAT_RGB24;
        break;
    default:
        g_assert_not_reached();
        break;
    }

    stride = cairo_format_stride_for_width(kind, ds->surface->width);
    g_assert_cmpint(ds->surface->linesize, ==, stride);

    s->surface = cairo_image_surface_create_for_data(ds->surface->data,
                                                     kind,
                                                     ds->surface->width,
                                                     ds->surface->height,
                                                     ds->surface->linesize);

    gtk_widget_set_size_request(s->drawing_area,
                                ds->surface->width,
                                ds->surface->height);
}

static void gd_update_caption(GtkDisplayState *s)
{
    const char *status = "";
    gchar *title;

    if (!runstate_is_running()) {
        status = " [Stopped]";
    }

    if (qemu_name) {
        title = g_strdup_printf("QEMU (%s)%s", qemu_name, status);
    } else {
        title = g_strdup_printf("QEMU%s", status);
    }
        
    gtk_window_set_title(GTK_WINDOW(s->window), title);

    g_free(title);
}

static void gd_change_runstate(void *opaque, int running, RunState state)
{
    GtkDisplayState *s = opaque;

    gd_update_caption(s);
}

static gboolean gd_draw_event(GtkWidget *widget, cairo_t *cr, void *opaque)
{
    GtkDisplayState *s = opaque;

    cairo_set_source_surface(cr, s->surface, 0, 0);
    cairo_paint(cr);

    return TRUE;
}

static gboolean gd_expose_event(GtkWidget *widget, GdkEventExpose *expose,
                                void *opaque)
{
    cairo_t *cr;
    gboolean ret;

    cr = gdk_cairo_create(gtk_widget_get_window(widget));
    cairo_rectangle(cr,
                    expose->area.x,
                    expose->area.y,
                    expose->area.width,
                    expose->area.height);
    cairo_clip(cr);

    ret = gd_draw_event(widget, cr, opaque);

    cairo_destroy(cr);

    return ret;
}

static gboolean gd_motion_event(GtkWidget *widget, GdkEventMotion *motion,
                                void *opaque)
{
    GtkDisplayState *s = opaque;
    int dx, dy;

    if (kbd_mouse_is_absolute()) {
        dx = motion->x * 0x7FFF / (s->ds->surface->width - 1);
        dy = motion->y * 0x7FFF / (s->ds->surface->height - 1);
    } else {
        dx = motion->x - s->last_x;
        dy = motion->y - s->last_y;
    }

    s->last_x = motion->x;
    s->last_y = motion->y;

    kbd_mouse_event(dx, dy, 0, s->button_mask);
        
    return TRUE;
}

static gboolean gd_button_event(GtkWidget *widget, GdkEventButton *button,
                                void *opaque)
{
    GtkDisplayState *s = opaque;
    int dx, dy;
    int n;

    if (button->button == 1) {
        n = 0x01;
    } else if (button->button == 2) {
        n = 0x04;
    } else if (button->button == 3) {
        n = 0x02;
    } else {
        n = 0x00;
    }

    if (button->type == GDK_BUTTON_PRESS) {
        s->button_mask |= n;
    } else if (button->type == GDK_BUTTON_RELEASE) {
        s->button_mask &= ~n;
    }

    if (kbd_mouse_is_absolute()) {
        dx = s->last_x * 0x7FFF / (s->ds->surface->width - 1);
        dy = s->last_y * 0x7FFF / (s->ds->surface->height - 1);
    } else {
        dx = 0;
        dy = 0;
    }

    kbd_mouse_event(dx, dy, 0, s->button_mask);
        
    return TRUE;
}

static gboolean gd_key_event(GtkWidget *widget, GdkEventKey *key, void *opaque)
{
    int gdk_keycode;
    int qemu_keycode;

    gdk_keycode = key->hardware_keycode;

    if (gdk_keycode < 9) {
        qemu_keycode = 0;
    } else if (gdk_keycode < 97) {
        qemu_keycode = gdk_keycode - 8;
    } else if (gdk_keycode < 158) {
        qemu_keycode = translate_evdev_keycode(gdk_keycode - 97);
    } else if (gdk_keycode == 208) { /* Hiragana_Katakana */
        qemu_keycode = 0x70;
    } else if (gdk_keycode == 211) { /* backslash */
        qemu_keycode = 0x73;
    } else {
        qemu_keycode = 0;
    }

    dprintf("translated GDK keycode %d to QEMU keycode %d (%s)\n",
            gdk_keycode, qemu_keycode,
            (key->type == GDK_KEY_PRESS) ? "down" : "up");

    if (qemu_keycode & SCANCODE_GREY) {
        kbd_put_keycode(SCANCODE_EMUL0);
    }

    if (key->type == GDK_KEY_PRESS) {
        kbd_put_keycode(qemu_keycode & SCANCODE_KEYCODEMASK);
    } else if (key->type == GDK_KEY_RELEASE) {
        kbd_put_keycode(qemu_keycode | SCANCODE_UP);
    } else {
        g_assert_not_reached();
    }

    return TRUE;
}

void gtk_display_init(DisplayState *ds)
{
    GtkDisplayState *s = g_malloc0(sizeof(*s));

    gtk_init(NULL, NULL);

    ds->opaque = s;
    s->ds = ds;
    s->dcl.dpy_update = gd_update;
    s->dcl.dpy_resize = gd_resize;
    s->dcl.dpy_refresh = gd_refresh;
    register_displaychangelistener(ds, &s->dcl);

    s->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    s->drawing_area = gtk_drawing_area_new();

    gtk_window_set_resizable(GTK_WINDOW(s->window), FALSE);
    g_signal_connect(s->window, "delete-event",
                     G_CALLBACK(gd_window_close), s);

    g_signal_connect(s->drawing_area, "expose-event",
                     G_CALLBACK(gd_expose_event), s);
    g_signal_connect(s->drawing_area, "motion-notify-event",
                     G_CALLBACK(gd_motion_event), s);
    g_signal_connect(s->drawing_area, "button-press-event",
                     G_CALLBACK(gd_button_event), s);
    g_signal_connect(s->drawing_area, "button-release-event",
                     G_CALLBACK(gd_button_event), s);
    g_signal_connect(s->drawing_area, "key-press-event",
                     G_CALLBACK(gd_key_event), s);
    g_signal_connect(s->drawing_area, "key-release-event",
                     G_CALLBACK(gd_key_event), s);

    gtk_widget_add_events(s->drawing_area,
                          GDK_POINTER_MOTION_MASK |
                          GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK |
                          GDK_BUTTON_MOTION_MASK |
                          GDK_SCROLL_MASK |
                          GDK_KEY_PRESS_MASK);
    gtk_widget_set_double_buffered(s->drawing_area, FALSE);
    gtk_widget_set_can_focus(s->drawing_area, TRUE);

    qemu_add_vm_change_state_handler(gd_change_runstate, s);
    gd_update_caption(s);

    gtk_container_add(GTK_CONTAINER(s->window), s->drawing_area);

    gtk_widget_show_all(s->window);

    global_state = s;
}
