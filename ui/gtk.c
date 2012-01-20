#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <vte/vte.h>

#include "qemu-common.h"
#include "console.h"
#include "sysemu.h"
#include "qmp-commands.h"
#include "x_keymap.h"
#include "keymaps.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <pty.h>

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
    int i;

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
    for (i = 0; i < s->nb_vcs; i++) {
        gtk_widget_set_size_request(s->vc[i].scrolled_window,
                                    ds->surface->width,
                                    ds->surface->height);
    }
        
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

    return FALSE;
}

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

static void gd_vc_set_echo(CharDriverState *chr, bool echo)
{
}

static int gd_vc_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    VirtualConsole *vc = chr->opaque;

    return write(vc->fd, buf, len);
}

static int nb_vcs;
CharDriverState *vcs[MAX_VCS];

static int gd_vc_handler(QemuOpts *opts, CharDriverState **chrp)
{
    CharDriverState *chr;

    chr = g_malloc0(sizeof(*chr));
    chr->chr_set_echo = gd_vc_set_echo;
    chr->chr_write = gd_vc_chr_write;

    *chrp = chr;

    vcs[nb_vcs++] = chr;

    return 0;
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
    GtkAdjustment *adjustment;
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

    adjustment = vte_terminal_get_adjustment(VTE_TERMINAL(vc->terminal));

    scrolled_window = gtk_scrolled_window_new(NULL, adjustment);

    gtk_container_add(GTK_CONTAINER(scrolled_window), vc->terminal);

    vc->fd = slave_fd;
    vc->chr->opaque = vc;
    vc->scrolled_window = scrolled_window;

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(vc->scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

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

void gtk_display_init(DisplayState *ds)
{
    GtkDisplayState *s = g_malloc0(sizeof(*s));
    GtkStockItem item;
    GtkAccelGroup *accel_group;
    GSList *group = NULL;
    int i;
    GtkWidget *separator;

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

    accel_group = gtk_accel_group_new();
    s->file_menu = gtk_menu_new();
    gtk_menu_set_accel_group(GTK_MENU(s->file_menu), accel_group);
    s->file_menu_item = gtk_menu_item_new_with_mnemonic("_File");

    s->quit_item = gtk_image_menu_item_new_from_stock(GTK_STOCK_QUIT, NULL);
    gtk_stock_lookup(GTK_STOCK_QUIT, &item);
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->quit_item),
                                 "<QEMU>/File/Quit");
    gtk_accel_map_add_entry("<QEMU>/File/Quit", item.keyval, item.modifier);

    s->view_menu = gtk_menu_new();
    gtk_menu_set_accel_group(GTK_MENU(s->view_menu), accel_group);
    s->view_menu_item = gtk_menu_item_new_with_mnemonic("_View");

    s->vga_item = gtk_radio_menu_item_new_with_mnemonic(group, "_VGA");
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(s->vga_item));
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->vga_item),
                                 "<QEMU>/View/VGA");
    gtk_accel_map_add_entry("<QEMU>/View/VGA", GDK_KEY_1, GDK_CONTROL_MASK | GDK_MOD1_MASK);

    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook), s->drawing_area, gtk_label_new("VGA"));
    gtk_menu_append(GTK_MENU(s->view_menu), s->vga_item);

    for (i = 0; i < nb_vcs; i++) {
        VirtualConsole *vc = &s->vc[i];

        group = gd_vc_init(s, vc, i, group);
        s->nb_vcs++;
    }

    separator = gtk_separator_menu_item_new();
    gtk_menu_append(GTK_MENU(s->view_menu), separator);

    s->show_tabs_item = gtk_check_menu_item_new_with_mnemonic("Show _Tabs");
    gtk_menu_append(GTK_MENU(s->view_menu), s->show_tabs_item);
    g_signal_connect(s->show_tabs_item, "activate",
                     G_CALLBACK(gd_menu_show_tabs), s);

    g_object_set_data(G_OBJECT(s->window), "accel_group", accel_group);
    gtk_window_add_accel_group(GTK_WINDOW(s->window), accel_group);


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

    g_signal_connect(s->quit_item, "activate",
                     G_CALLBACK(gd_menu_quit), s);
    g_signal_connect(s->vga_item, "activate",
                     G_CALLBACK(gd_menu_switch_vc), s);

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

    qemu_add_vm_change_state_handler(gd_change_runstate, s);
    gd_update_caption(s);

    gtk_menu_append(GTK_MENU(s->file_menu), s->quit_item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(s->file_menu_item), s->file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(s->menu_bar), s->file_menu_item);

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(s->view_menu_item), s->view_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(s->menu_bar), s->view_menu_item);

    gtk_box_pack_start(GTK_BOX(s->vbox), s->menu_bar, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(s->vbox), s->notebook, FALSE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(s->window), s->vbox);

    gtk_widget_show_all(s->window);

    global_state = s;
}
