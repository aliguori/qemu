/*
 * GTK UI
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * Portions from gtk-vnc:
 *
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2009-2010 Daniel P. Berrange <dan@berrange.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#define GETTEXT_PACKAGE "qemu"
#define LOCALEDIR "po"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>
#include <vte/vte.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <pty.h>
#include <math.h>

#include "qemu-common.h"
#include "console.h"
#include "sysemu.h"
#include "qmp-commands.h"
#include "x_keymap.h"
#include "keymaps.h"

//#define DEBUG_GTK

#ifdef DEBUG_GTK
#define dprintf(fmt, ...) printf(fmt, ## __VA_ARGS__)
#else
#define dprintf(fmt, ...) do { } while (0)
#endif

#define MAX_VCS 10

typedef struct VirtualConsole
{
    GtkWidget *menu_item;
    GtkWidget *terminal;
    GtkWidget *scrolled_window;
    CharDriverState *chr;
    int fd;
} VirtualConsole;

typedef struct GtkDisplayState
{
    GtkWidget *window;

    GtkWidget *menu_bar;

    GtkWidget *file_menu_item;
    GtkWidget *file_menu;
    GtkWidget *quit_item;

    GtkWidget *view_menu_item;
    GtkWidget *view_menu;
    GtkWidget *full_screen_item;
    GtkWidget *zoom_in_item;
    GtkWidget *zoom_out_item;
    GtkWidget *grab_item;
    GtkWidget *vga_item;

    int nb_vcs;
    VirtualConsole vc[MAX_VCS];

    GtkWidget *show_tabs_item;

    GtkWidget *vbox;
    GtkWidget *notebook;
    GtkWidget *drawing_area;
    cairo_surface_t *surface;
    DisplayChangeListener dcl;
    DisplayState *ds;
    int button_mask;
    int last_x;
    int last_y;

    double scale_x;
    double scale_y;
    gboolean full_screen;

    GdkCursor *null_cursor;
    Notifier mouse_mode_notifier;
} GtkDisplayState;

static GtkDisplayState *global_state;

/** Utility Functions **/

static bool gd_is_grab_active(GtkDisplayState *s)
{
    return gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(s->grab_item));
}

static void gd_update_cursor(GtkDisplayState *s, gboolean override)
{
    GdkWindow *window;
    bool on_vga;

    window = gtk_widget_get_window(GTK_WIDGET(s->drawing_area));

    on_vga = (gtk_notebook_get_current_page(GTK_NOTEBOOK(s->notebook)) == 0);

    if ((override || on_vga) &&
        (s->full_screen || kbd_mouse_is_absolute() || gd_is_grab_active(s))) {
	gdk_window_set_cursor(window, s->null_cursor);
    } else {
	gdk_window_set_cursor(window, NULL);
    }
}

static void gd_update_caption(GtkDisplayState *s)
{
    const char *status = "";
    gchar *title;
    const char *grab = "";

    if (gd_is_grab_active(s)) {
        grab = " - Press Ctrl+Alt+G to release grab";
    }

    if (!runstate_is_running()) {
        status = " [Stopped]";
    }

    if (qemu_name) {
        title = g_strdup_printf("QEMU (%s)%s%s", qemu_name, status, grab);
    } else {
        title = g_strdup_printf("QEMU%s%s", status, grab);
    }
        
    gtk_window_set_title(GTK_WINDOW(s->window), title);

    g_free(title);
}

/** DisplayState Callbacks **/

static void gd_update(DisplayState *ds, int x, int y, int w, int h)
{
    GtkDisplayState *s = ds->opaque;
    int x1, x2, y1, y2;

    dprintf("update(x=%d, y=%d, w=%d, h=%d)\n", x, y, w, h);

    x1 = floor(x * s->scale_x);
    y1 = floor(y * s->scale_y);

    x2 = ceil(x * s->scale_x + w * s->scale_x);
    y2 = ceil(y * s->scale_y + h * s->scale_y);

    gtk_widget_queue_draw_area(s->drawing_area, x1, y1, (x2 - x1), (y2 - y1));
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

    if (!s->full_screen) {
        gtk_widget_set_size_request(s->drawing_area,
                                    ds->surface->width * s->scale_x,
                                    ds->surface->height * s->scale_y);
    }
}

/** QEMU Events **/

static void gd_change_runstate(void *opaque, int running, RunState state)
{
    GtkDisplayState *s = opaque;

    gd_update_caption(s);
}

static void gd_mouse_mode_change(Notifier *notify, void *data)
{
    gd_update_cursor(container_of(notify, GtkDisplayState, mouse_mode_notifier),
                     FALSE);
}

/** GTK Events **/

static gboolean gd_window_close(GtkWidget *widget, GdkEvent *event,
                                void *opaque)
{
    if (!no_quit) {
        qmp_quit(NULL);
        return FALSE;
    }

    return TRUE;
}

static gboolean gd_draw_event(GtkWidget *widget, cairo_t *cr, void *opaque)
{
    GtkDisplayState *s = opaque;
    int ww, wh;
    int fbw, fbh;

    fbw = s->ds->surface->width;
    fbh = s->ds->surface->height;

    gdk_drawable_get_size(gtk_widget_get_window(widget), &ww, &wh);

    cairo_rectangle(cr, 0, 0, ww, wh);

    if (ww != fbw || wh != fbh) {
        s->scale_x = (double)ww / fbw;
        s->scale_y = (double)wh / fbh;
        cairo_scale(cr, s->scale_x, s->scale_y);
    } else {
        s->scale_x = 1.0;
        s->scale_y = 1.0;
    }

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
    int x, y;

    x = motion->x / s->scale_x;
    y = motion->y / s->scale_y;

    if (kbd_mouse_is_absolute()) {
        dx = x * 0x7FFF / (s->ds->surface->width - 1);
        dy = y * 0x7FFF / (s->ds->surface->height - 1);
    } else if (s->last_x == -1 || s->last_y == -1) {
        dx = 0;
        dy = 0;
    } else {
        dx = x - s->last_x;
        dy = y - s->last_y;
    }

    s->last_x = x;
    s->last_y = y;

    if (kbd_mouse_is_absolute() || gd_is_grab_active(s)) {
        kbd_mouse_event(dx, dy, 0, s->button_mask);
    }

    if (!kbd_mouse_is_absolute() && gd_is_grab_active(s)) {
        GdkDrawable *drawable = GDK_DRAWABLE(gtk_widget_get_window(s->drawing_area));
        GdkDisplay *display = gdk_drawable_get_display(drawable);
        GdkScreen *screen = gdk_drawable_get_screen(drawable);
        int x = (int)motion->x_root;
        int y = (int)motion->y_root;

        /* In relative mode check to see if client pointer hit
         * one of the screen edges, and if so move it back by
         * 200 pixels. This is important because the pointer
         * in the server doesn't correspond 1-for-1, and so
         * may still be only half way across the screen. Without
         * this warp, the server pointer would thus appear to hit
         * an invisible wall */
        if (x == 0) {
            x += 200;
        }
        if (y == 0) {
            y += 200;
        }
        if (x == (gdk_screen_get_width(screen) - 1)) {
            x -= 200;
        }
        if (y == (gdk_screen_get_height(screen) - 1)) {
            y -= 200;
        }

        if (x != (int)motion->x_root || y != (int)motion->y_root) {
            gdk_display_warp_pointer(display, screen, x, y);
            s->last_x = -1;
            s->last_y = -1;
            return FALSE;
        }
    }        
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

/** Window Menu Actions **/

static void gd_menu_quit(GtkMenuItem *item, void *opaque)
{
    qmp_quit(NULL);
}

static void gd_menu_switch_vc(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;

    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(s->vga_item))) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(s->notebook), 0);
    } else {
        int i;

        for (i = 0; i < s->nb_vcs; i++) {
            if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(s->vc[i].menu_item))) {
                gtk_notebook_set_current_page(GTK_NOTEBOOK(s->notebook), i + 1);
                break;
            }
        }
    }
}

static void gd_menu_show_tabs(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;

    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(s->show_tabs_item))) {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), TRUE);
    } else {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
    }
}

static void gd_menu_full_screen(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;

    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(s->full_screen_item))) {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
        gtk_widget_set_size_request(s->menu_bar, 0, 0);
        gtk_widget_set_size_request(s->drawing_area, -1, -1);
        gtk_window_set_resizable(GTK_WINDOW(s->window), TRUE);
        gtk_window_fullscreen(GTK_WINDOW(s->window));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->grab_item), TRUE);
        s->full_screen = TRUE;
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(s->window));
        gd_menu_show_tabs(GTK_MENU_ITEM(s->show_tabs_item), s);
        gtk_widget_set_size_request(s->menu_bar, -1, -1);
        gtk_widget_set_size_request(s->drawing_area, s->ds->surface->width, s->ds->surface->height);
        gtk_window_set_resizable(GTK_WINDOW(s->window), FALSE);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->grab_item), FALSE);
        s->full_screen = FALSE;
    }

    gd_update_cursor(s, FALSE);
}

static void gd_menu_zoom_in(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;

    s->scale_x += .25;
    s->scale_y += .25;

    gd_resize(s->ds);
}

static void gd_menu_zoom_out(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;

    s->scale_x -= .25;
    s->scale_y -= .25;

    s->scale_x = MAX(s->scale_x, .25);
    s->scale_y = MAX(s->scale_y, .25);

    gd_resize(s->ds);
}

static void gd_menu_grab_input(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;

    if (gd_is_grab_active(s)) {
	gdk_keyboard_grab(gtk_widget_get_window(GTK_WIDGET(s->drawing_area)),
			  FALSE,
			  GDK_CURRENT_TIME);
	gdk_pointer_grab(gtk_widget_get_window(GTK_WIDGET(s->drawing_area)),
			 FALSE, /* All events to come to our window directly */
			 GDK_POINTER_MOTION_MASK |
			 GDK_BUTTON_PRESS_MASK |
			 GDK_BUTTON_RELEASE_MASK |
			 GDK_BUTTON_MOTION_MASK |
			 GDK_SCROLL_MASK,
			 NULL, /* Allow cursor to move over entire desktop */
                         s->null_cursor,
			 GDK_CURRENT_TIME);
    } else {
	gdk_keyboard_ungrab(GDK_CURRENT_TIME);
	gdk_pointer_ungrab(GDK_CURRENT_TIME);
    }

    gd_update_caption(s);
    gd_update_cursor(s, FALSE);
}

static void gd_change_page(GtkNotebook *nb, gpointer arg1, guint arg2,
                           gpointer data)
{
    GtkDisplayState *s = data;
    guint last_page;
    gboolean on_vga;

    if (!gtk_widget_get_realized(s->notebook)) {
        return;
    }

    last_page = gtk_notebook_get_current_page(nb);

    if (last_page) {
        gtk_widget_set_size_request(s->vc[last_page - 1].terminal, -1, -1);
    }

    on_vga = arg2 == 0;

    if (!on_vga) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->grab_item),
                                       FALSE);
    } else if (s->full_screen) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->grab_item),
                                       TRUE);
    }

    if (arg2 == 0) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->vga_item), TRUE);
    } else {
        VirtualConsole *vc = &s->vc[arg2 - 1];
        VteTerminal *term = VTE_TERMINAL(vc->terminal);
        int width, height;

        width = 80 * vte_terminal_get_char_width(term);
        height = 25 * vte_terminal_get_char_height(term);

        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(vc->menu_item), TRUE);
        gtk_widget_set_size_request(vc->terminal, width, height);
    }        

    gtk_widget_set_sensitive(s->grab_item, on_vga);

    gd_update_cursor(s, TRUE);
}

/** Virtual Console Callbacks **/

static int gd_vc_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    VirtualConsole *vc = chr->opaque;

    return write(vc->fd, buf, len);
}

static int nb_vcs;
static CharDriverState *vcs[MAX_VCS];

static CharDriverState *gd_vc_handler(QemuOpts *opts)
{
    CharDriverState *chr;

    chr = g_malloc0(sizeof(*chr));
    chr->chr_write = gd_vc_chr_write;

    vcs[nb_vcs++] = chr;

    return chr;
}

void early_gtk_display_init(void)
{
    register_vc_handler(gd_vc_handler);
}

static gboolean gd_vc_in(GIOChannel *chan, GIOCondition cond, void *opaque)
{
    VirtualConsole *vc = opaque;
    uint8_t buffer[1024];
    ssize_t len;

    len = read(vc->fd, buffer, sizeof(buffer));
    if (len <= 0) {
        return FALSE;
    }

    qemu_chr_be_write(vc->chr, buffer, len);

    return TRUE;
}

static GSList *gd_vc_init(GtkDisplayState *s, VirtualConsole *vc, int index, GSList *group)
{
    const char *label;
    char buffer[32];
    char path[32];
    VtePty *pty;
    GIOChannel *chan;
    GtkWidget *scrolled_window;
    GtkAdjustment *hadjustment, *vadjustment;
    int master_fd, slave_fd, ret;
    struct termios tty;

    snprintf(buffer, sizeof(buffer), "vc%d", index);
    snprintf(path, sizeof(path), "<QEMU>/View/VC%d", index);

    vc->chr = vcs[index];

    if (vc->chr->label) {
        label = vc->chr->label;
    } else {
        label = buffer;
    }

    vc->menu_item = gtk_radio_menu_item_new_with_mnemonic(group, label);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(vc->menu_item));
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(vc->menu_item), path);
    gtk_accel_map_add_entry(path, GDK_KEY_2 + index, GDK_CONTROL_MASK | GDK_MOD1_MASK);

    vc->terminal = vte_terminal_new();

    ret = openpty(&master_fd, &slave_fd, NULL, NULL, NULL);
    g_assert(ret != -1);

    /* Set raw attributes on the pty. */
    tcgetattr(slave_fd, &tty);
    cfmakeraw(&tty);
    tcsetattr(slave_fd, TCSAFLUSH, &tty);

    pty = vte_pty_new_foreign(master_fd, NULL);

    vte_terminal_set_pty_object(VTE_TERMINAL(vc->terminal), pty);

    vte_terminal_set_scrollback_lines(VTE_TERMINAL(vc->terminal), -1);

    vadjustment = vte_terminal_get_adjustment(VTE_TERMINAL(vc->terminal));
    hadjustment = NULL;

    scrolled_window = gtk_scrolled_window_new(NULL, vadjustment);
    gtk_container_add(GTK_CONTAINER(scrolled_window), vc->terminal);

    vte_terminal_set_size(VTE_TERMINAL(vc->terminal), 80, 25);

    vc->fd = slave_fd;
    vc->chr->opaque = vc;
    vc->scrolled_window = scrolled_window;

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(vc->scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook), scrolled_window, gtk_label_new(label));
    g_signal_connect(vc->menu_item, "activate",
                     G_CALLBACK(gd_menu_switch_vc), s);

    gtk_menu_append(GTK_MENU(s->view_menu), vc->menu_item);

    qemu_chr_generic_open(vc->chr);
    if (vc->chr->init) {
        vc->chr->init(vc->chr);
    }

    chan = g_io_channel_unix_new(vc->fd);
    g_io_add_watch(chan, G_IO_IN, gd_vc_in, vc);

    return group;
}

/** Window Creation **/

static void gd_connect_signals(GtkDisplayState *s)
{
    g_signal_connect(s->show_tabs_item, "activate",
                     G_CALLBACK(gd_menu_show_tabs), s);

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

    g_signal_connect(s->quit_item, "activate",
                     G_CALLBACK(gd_menu_quit), s);
    g_signal_connect(s->full_screen_item, "activate",
                     G_CALLBACK(gd_menu_full_screen), s);
    g_signal_connect(s->zoom_in_item, "activate",
                     G_CALLBACK(gd_menu_zoom_in), s);
    g_signal_connect(s->zoom_out_item, "activate",
                     G_CALLBACK(gd_menu_zoom_out), s);
    g_signal_connect(s->vga_item, "activate",
                     G_CALLBACK(gd_menu_switch_vc), s);
    g_signal_connect(s->grab_item, "activate",
                     G_CALLBACK(gd_menu_grab_input), s);
    g_signal_connect(s->notebook, "switch-page",
                     G_CALLBACK(gd_change_page), s);
}

static void gd_create_menus(GtkDisplayState *s)
{
    GtkStockItem item;
    GtkAccelGroup *accel_group;
    GSList *group = NULL;
    GtkWidget *separator;
    int i;

    accel_group = gtk_accel_group_new();
    s->file_menu = gtk_menu_new();
    gtk_menu_set_accel_group(GTK_MENU(s->file_menu), accel_group);
    s->file_menu_item = gtk_menu_item_new_with_mnemonic(_("_File"));

    s->quit_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_QUIT, NULL);
    gtk_stock_lookup(GTK_STOCK_QUIT, &item);
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->quit_item),
                                 "<QEMU>/File/Quit");
    gtk_accel_map_add_entry("<QEMU>/File/Quit", item.keyval, item.modifier);

    s->view_menu = gtk_menu_new();
    gtk_menu_set_accel_group(GTK_MENU(s->view_menu), accel_group);
    s->view_menu_item = gtk_menu_item_new_with_mnemonic(_("_View"));

    s->full_screen_item = gtk_check_menu_item_new_with_mnemonic(_("_Full Screen"));
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->full_screen_item),
                                 "<QEMU>/View/Full Screen");
    gtk_accel_map_add_entry("<QEMU>/View/Full Screen", GDK_KEY_f, GDK_CONTROL_MASK | GDK_MOD1_MASK);
    gtk_menu_append(GTK_MENU(s->view_menu), s->full_screen_item);

    separator = gtk_separator_menu_item_new();
    gtk_menu_append(GTK_MENU(s->view_menu), separator);

    s->zoom_in_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_ZOOM_IN, NULL);
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->zoom_in_item),
                                 "<QEMU>/View/Zoom In");
    gtk_accel_map_add_entry("<QEMU>/View/Zoom In", GDK_KEY_plus, GDK_CONTROL_MASK | GDK_MOD1_MASK);
    gtk_menu_append(GTK_MENU(s->view_menu), s->zoom_in_item);

    s->zoom_out_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_ZOOM_OUT, NULL);
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->zoom_out_item),
                                 "<QEMU>/View/Zoom Out");
    gtk_accel_map_add_entry("<QEMU>/View/Zoom Out", GDK_KEY_minus, GDK_CONTROL_MASK | GDK_MOD1_MASK);
    gtk_menu_append(GTK_MENU(s->view_menu), s->zoom_out_item);

    separator = gtk_separator_menu_item_new();
    gtk_menu_append(GTK_MENU(s->view_menu), separator);

    s->grab_item = gtk_check_menu_item_new_with_mnemonic(_("_Grab Input"));
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->grab_item),
                                 "<QEMU>/View/Grab Input");
    gtk_accel_map_add_entry("<QEMU>/View/Grab Input", GDK_KEY_g, GDK_CONTROL_MASK | GDK_MOD1_MASK);
    gtk_menu_append(GTK_MENU(s->view_menu), s->grab_item);

    separator = gtk_separator_menu_item_new();
    gtk_menu_append(GTK_MENU(s->view_menu), separator);

    s->vga_item = gtk_radio_menu_item_new_with_mnemonic(group, "_VGA");
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(s->vga_item));
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->vga_item),
                                 "<QEMU>/View/VGA");
    gtk_accel_map_add_entry("<QEMU>/View/VGA", GDK_KEY_1, GDK_CONTROL_MASK | GDK_MOD1_MASK);
    gtk_menu_append(GTK_MENU(s->view_menu), s->vga_item);

    for (i = 0; i < nb_vcs; i++) {
        VirtualConsole *vc = &s->vc[i];

        group = gd_vc_init(s, vc, i, group);
        s->nb_vcs++;
    }

    separator = gtk_separator_menu_item_new();
    gtk_menu_append(GTK_MENU(s->view_menu), separator);

    s->show_tabs_item = gtk_check_menu_item_new_with_mnemonic(_("Show _Tabs"));
    gtk_menu_append(GTK_MENU(s->view_menu), s->show_tabs_item);

    g_object_set_data(G_OBJECT(s->window), "accel_group", accel_group);
    gtk_window_add_accel_group(GTK_WINDOW(s->window), accel_group);

    gtk_menu_append(GTK_MENU(s->file_menu), s->quit_item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(s->file_menu_item), s->file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(s->menu_bar), s->file_menu_item);

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(s->view_menu_item), s->view_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(s->menu_bar), s->view_menu_item);
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
    s->vbox = gtk_vbox_new(FALSE, 0);
    s->notebook = gtk_notebook_new();
    s->drawing_area = gtk_drawing_area_new();
    s->menu_bar = gtk_menu_bar_new();

    s->scale_x = 1.0;
    s->scale_y = 1.0;

    setlocale(LC_ALL, "");
    bindtextdomain("qemu", CONFIG_QEMU_PREFIX "/share/locale");
    textdomain("qemu");

    s->null_cursor = gdk_cursor_new(GDK_BLANK_CURSOR);

    s->mouse_mode_notifier.notify = gd_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&s->mouse_mode_notifier);
    qemu_add_vm_change_state_handler(gd_change_runstate, s);

    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook), s->drawing_area, gtk_label_new("VGA"));

    gd_create_menus(s);

    gd_connect_signals(s);

    gtk_widget_add_events(s->drawing_area,
                          GDK_POINTER_MOTION_MASK |
                          GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK |
                          GDK_BUTTON_MOTION_MASK |
                          GDK_SCROLL_MASK |
                          GDK_KEY_PRESS_MASK);
    gtk_widget_set_double_buffered(s->drawing_area, FALSE);
    gtk_widget_set_can_focus(s->drawing_area, TRUE);

    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(s->notebook), FALSE);

    gtk_window_set_resizable(GTK_WINDOW(s->window), FALSE);

    gd_update_caption(s);

    gtk_box_pack_start(GTK_BOX(s->vbox), s->menu_bar, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(s->vbox), s->notebook, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(s->window), s->vbox);

    gtk_widget_show_all(s->window);

    global_state = s;
}
