/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu-common.h"
#include "monitor/monitor.h"
#include "ui/console.h"
#include "sysemu/sysemu.h"
#include "qemu/timer.h"
#include "char/char.h"
#include "hw/usb.h"
#include "hw/baum.h"
#include "hw/msmouse.h"
#include "qmp-commands.h"

#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <zlib.h>

#ifndef _WIN32
#include <sys/times.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <netdb.h>
#include <sys/select.h>
#ifdef CONFIG_BSD
#include <sys/stat.h>
#if defined(__GLIBC__)
#include <pty.h>
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)
#include <libutil.h>
#else
#include <util.h>
#endif
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#include <dev/ppbus/ppi.h>
#include <dev/ppbus/ppbconf.h>
#elif defined(__DragonFly__)
#include <dev/misc/ppi/ppi.h>
#include <bus/ppbus/ppbconf.h>
#endif
#else
#ifdef __linux__
#include <pty.h>

#include <linux/ppdev.h>
#include <linux/parport.h>
#endif
#ifdef __sun__
#include <sys/stat.h>
#include <sys/ethernet.h>
#include <sys/sockio.h>
#include <netinet/arp.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h> // must come after ip.h
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <net/if.h>
#include <syslog.h>
#include <stropts.h>
#endif
#endif
#endif

#include "qemu/sockets.h"
#include "ui/qemu-spice.h"

#define READ_BUF_LEN 4096

/***********************************************************/
/* character device */

static QTAILQ_HEAD(CharDriverStateHead, CharDriverState) chardevs =
    QTAILQ_HEAD_INITIALIZER(chardevs);

void qemu_chr_be_event(CharDriverState *s, int event)
{
    /* Keep track if the char device is open */
    switch (event) {
        case CHR_EVENT_OPENED:
            s->opened = 1;
            break;
        case CHR_EVENT_CLOSED:
            s->opened = 0;
            break;
    }

    if (!s->chr_event)
        return;
    s->chr_event(s->handler_opaque, event);
}

static void qemu_chr_fire_open_event(void *opaque)
{
    CharDriverState *s = opaque;
    qemu_chr_be_event(s, CHR_EVENT_OPENED);
    qemu_free_timer(s->open_timer);
    s->open_timer = NULL;
}

void qemu_chr_generic_open(CharDriverState *s)
{
    if (s->open_timer == NULL) {
        s->open_timer = qemu_new_timer_ms(rt_clock,
                                          qemu_chr_fire_open_event, s);
        qemu_mod_timer(s->open_timer, qemu_get_clock_ms(rt_clock) - 1);
    }
}

int qemu_chr_fe_write(CharDriverState *s, const uint8_t *buf, int len)
{
    return s->chr_write(s, buf, len);
}

int qemu_chr_fe_ioctl(CharDriverState *s, int cmd, void *arg)
{
    if (!s->chr_ioctl)
        return -ENOTSUP;
    return s->chr_ioctl(s, cmd, arg);
}

int qemu_chr_be_can_write(CharDriverState *s)
{
    if (!s->chr_can_read)
        return 0;
    return s->chr_can_read(s->handler_opaque);
}

void qemu_chr_be_write(CharDriverState *s, uint8_t *buf, int len)
{
    if (s->chr_read) {
        s->chr_read(s->handler_opaque, buf, len);
    }
}

int qemu_chr_fe_get_msgfd(CharDriverState *s)
{
    return s->get_msgfd ? s->get_msgfd(s) : -1;
}

int qemu_chr_add_client(CharDriverState *s, int fd)
{
    return s->chr_add_client ? s->chr_add_client(s, fd) : -1;
}

void qemu_chr_accept_input(CharDriverState *s)
{
    if (s->chr_accept_input)
        s->chr_accept_input(s);
    qemu_notify_event();
}

void qemu_chr_fe_printf(CharDriverState *s, const char *fmt, ...)
{
    char buf[READ_BUF_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    qemu_chr_fe_write(s, (uint8_t *)buf, strlen(buf));
    va_end(ap);
}

void qemu_chr_add_handlers(CharDriverState *s,
                           IOCanReadHandler *fd_can_read,
                           IOReadHandler *fd_read,
                           IOEventHandler *fd_event,
                           void *opaque)
{
    if (!opaque && !fd_can_read && !fd_read && !fd_event) {
        /* chr driver being released. */
        ++s->avail_connections;
    }
    s->chr_can_read = fd_can_read;
    s->chr_read = fd_read;
    s->chr_event = fd_event;
    s->handler_opaque = opaque;
    if (s->chr_update_read_handler)
        s->chr_update_read_handler(s);

    /* We're connecting to an already opened device, so let's make sure we
       also get the open event */
    if (s->opened) {
        qemu_chr_generic_open(s);
    }
}

static int null_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    return len;
}

static CharDriverState *qemu_chr_open_null(QemuOpts *opts)
{
    CharDriverState *chr;

    chr = g_malloc0(sizeof(CharDriverState));
    chr->chr_write = null_chr_write;
    return chr;
}

/* MUX driver for serial I/O splitting */
#define MAX_MUX 4
#define MUX_BUFFER_SIZE 32	/* Must be a power of 2.  */
#define MUX_BUFFER_MASK (MUX_BUFFER_SIZE - 1)
typedef struct {
    IOCanReadHandler *chr_can_read[MAX_MUX];
    IOReadHandler *chr_read[MAX_MUX];
    IOEventHandler *chr_event[MAX_MUX];
    void *ext_opaque[MAX_MUX];
    CharDriverState *drv;
    int focus;
    int mux_cnt;
    int term_got_escape;
    int max_size;
    /* Intermediate input buffer allows to catch escape sequences even if the
       currently active device is not accepting any input - but only until it
       is full as well. */
    unsigned char buffer[MAX_MUX][MUX_BUFFER_SIZE];
    int prod[MAX_MUX];
    int cons[MAX_MUX];
    int timestamps;
    int linestart;
    int64_t timestamps_start;
} MuxDriver;


static int mux_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    MuxDriver *d = chr->opaque;
    int ret;
    if (!d->timestamps) {
        ret = d->drv->chr_write(d->drv, buf, len);
    } else {
        int i;

        ret = 0;
        for (i = 0; i < len; i++) {
            if (d->linestart) {
                char buf1[64];
                int64_t ti;
                int secs;

                ti = qemu_get_clock_ms(rt_clock);
                if (d->timestamps_start == -1)
                    d->timestamps_start = ti;
                ti -= d->timestamps_start;
                secs = ti / 1000;
                snprintf(buf1, sizeof(buf1),
                         "[%02d:%02d:%02d.%03d] ",
                         secs / 3600,
                         (secs / 60) % 60,
                         secs % 60,
                         (int)(ti % 1000));
                d->drv->chr_write(d->drv, (uint8_t *)buf1, strlen(buf1));
                d->linestart = 0;
            }
            ret += d->drv->chr_write(d->drv, buf+i, 1);
            if (buf[i] == '\n') {
                d->linestart = 1;
            }
        }
    }
    return ret;
}

static const char * const mux_help[] = {
    "% h    print this help\n\r",
    "% x    exit emulator\n\r",
    "% s    save disk data back to file (if -snapshot)\n\r",
    "% t    toggle console timestamps\n\r"
    "% b    send break (magic sysrq)\n\r",
    "% c    switch between console and monitor\n\r",
    "% %  sends %\n\r",
    NULL
};

int term_escape_char = 0x01; /* ctrl-a is used for escape */
static void mux_print_help(CharDriverState *chr)
{
    int i, j;
    char ebuf[15] = "Escape-Char";
    char cbuf[50] = "\n\r";

    if (term_escape_char > 0 && term_escape_char < 26) {
        snprintf(cbuf, sizeof(cbuf), "\n\r");
        snprintf(ebuf, sizeof(ebuf), "C-%c", term_escape_char - 1 + 'a');
    } else {
        snprintf(cbuf, sizeof(cbuf),
                 "\n\rEscape-Char set to Ascii: 0x%02x\n\r\n\r",
                 term_escape_char);
    }
    chr->chr_write(chr, (uint8_t *)cbuf, strlen(cbuf));
    for (i = 0; mux_help[i] != NULL; i++) {
        for (j=0; mux_help[i][j] != '\0'; j++) {
            if (mux_help[i][j] == '%')
                chr->chr_write(chr, (uint8_t *)ebuf, strlen(ebuf));
            else
                chr->chr_write(chr, (uint8_t *)&mux_help[i][j], 1);
        }
    }
}

static void mux_chr_send_event(MuxDriver *d, int mux_nr, int event)
{
    if (d->chr_event[mux_nr])
        d->chr_event[mux_nr](d->ext_opaque[mux_nr], event);
}

static int mux_proc_byte(CharDriverState *chr, MuxDriver *d, int ch)
{
    if (d->term_got_escape) {
        d->term_got_escape = 0;
        if (ch == term_escape_char)
            goto send_char;
        switch(ch) {
        case '?':
        case 'h':
            mux_print_help(chr);
            break;
        case 'x':
            {
                 const char *term =  "QEMU: Terminated\n\r";
                 chr->chr_write(chr,(uint8_t *)term,strlen(term));
                 exit(0);
                 break;
            }
        case 's':
            bdrv_commit_all();
            break;
        case 'b':
            qemu_chr_be_event(chr, CHR_EVENT_BREAK);
            break;
        case 'c':
            /* Switch to the next registered device */
            mux_chr_send_event(d, d->focus, CHR_EVENT_MUX_OUT);
            d->focus++;
            if (d->focus >= d->mux_cnt)
                d->focus = 0;
            mux_chr_send_event(d, d->focus, CHR_EVENT_MUX_IN);
            break;
        case 't':
            d->timestamps = !d->timestamps;
            d->timestamps_start = -1;
            d->linestart = 0;
            break;
        }
    } else if (ch == term_escape_char) {
        d->term_got_escape = 1;
    } else {
    send_char:
        return 1;
    }
    return 0;
}

static void mux_chr_accept_input(CharDriverState *chr)
{
    MuxDriver *d = chr->opaque;
    int m = d->focus;

    while (d->prod[m] != d->cons[m] &&
           d->chr_can_read[m] &&
           d->chr_can_read[m](d->ext_opaque[m])) {
        d->chr_read[m](d->ext_opaque[m],
                       &d->buffer[m][d->cons[m]++ & MUX_BUFFER_MASK], 1);
    }
}

static int mux_chr_can_read(void *opaque)
{
    CharDriverState *chr = opaque;
    MuxDriver *d = chr->opaque;
    int m = d->focus;

    if ((d->prod[m] - d->cons[m]) < MUX_BUFFER_SIZE)
        return 1;
    if (d->chr_can_read[m])
        return d->chr_can_read[m](d->ext_opaque[m]);
    return 0;
}

static void mux_chr_read(void *opaque, const uint8_t *buf, int size)
{
    CharDriverState *chr = opaque;
    MuxDriver *d = chr->opaque;
    int m = d->focus;
    int i;

    mux_chr_accept_input (opaque);

    for(i = 0; i < size; i++)
        if (mux_proc_byte(chr, d, buf[i])) {
            if (d->prod[m] == d->cons[m] &&
                d->chr_can_read[m] &&
                d->chr_can_read[m](d->ext_opaque[m]))
                d->chr_read[m](d->ext_opaque[m], &buf[i], 1);
            else
                d->buffer[m][d->prod[m]++ & MUX_BUFFER_MASK] = buf[i];
        }
}

static void mux_chr_event(void *opaque, int event)
{
    CharDriverState *chr = opaque;
    MuxDriver *d = chr->opaque;
    int i;

    /* Send the event to all registered listeners */
    for (i = 0; i < d->mux_cnt; i++)
        mux_chr_send_event(d, i, event);
}

static void mux_chr_update_read_handler(CharDriverState *chr)
{
    MuxDriver *d = chr->opaque;

    if (d->mux_cnt >= MAX_MUX) {
        fprintf(stderr, "Cannot add I/O handlers, MUX array is full\n");
        return;
    }
    d->ext_opaque[d->mux_cnt] = chr->handler_opaque;
    d->chr_can_read[d->mux_cnt] = chr->chr_can_read;
    d->chr_read[d->mux_cnt] = chr->chr_read;
    d->chr_event[d->mux_cnt] = chr->chr_event;
    /* Fix up the real driver with mux routines */
    if (d->mux_cnt == 0) {
        qemu_chr_add_handlers(d->drv, mux_chr_can_read, mux_chr_read,
                              mux_chr_event, chr);
    }
    if (d->focus != -1) {
        mux_chr_send_event(d, d->focus, CHR_EVENT_MUX_OUT);
    }
    d->focus = d->mux_cnt;
    d->mux_cnt++;
    mux_chr_send_event(d, d->focus, CHR_EVENT_MUX_IN);
}

static CharDriverState *qemu_chr_open_mux(CharDriverState *drv)
{
    CharDriverState *chr;
    MuxDriver *d;

    chr = g_malloc0(sizeof(CharDriverState));
    d = g_malloc0(sizeof(MuxDriver));

    chr->opaque = d;
    d->drv = drv;
    d->focus = -1;
    chr->chr_write = mux_chr_write;
    chr->chr_update_read_handler = mux_chr_update_read_handler;
    chr->chr_accept_input = mux_chr_accept_input;
    /* Frontend guest-open / -close notification is not support with muxes */
    chr->chr_guest_open = NULL;
    chr->chr_guest_close = NULL;

    /* Muxes are always open on creation */
    qemu_chr_generic_open(chr);

    return chr;
}


#ifdef _WIN32
int send_all(int fd, const void *buf, int len1)
{
    int ret, len;

    len = len1;
    while (len > 0) {
        ret = send(fd, buf, len, 0);
        if (ret < 0) {
            errno = WSAGetLastError();
            if (errno != WSAEWOULDBLOCK) {
                return -1;
            }
        } else if (ret == 0) {
            break;
        } else {
            buf += ret;
            len -= ret;
        }
    }
    return len1 - len;
}

#else

int send_all(int fd, const void *_buf, int len1)
{
    int ret, len;
    const uint8_t *buf = _buf;

    len = len1;
    while (len > 0) {
        ret = write(fd, buf, len);
        if (ret < 0) {
            if (errno != EINTR && errno != EAGAIN)
                return -1;
        } else if (ret == 0) {
            break;
        } else {
            buf += ret;
            len -= ret;
        }
    }
    return len1 - len;
}
#endif /* !_WIN32 */

#ifndef _WIN32

typedef struct {
    int fd_in, fd_out;
    int max_size;
} FDCharDriver;


static int fd_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    FDCharDriver *s = chr->opaque;
    return send_all(s->fd_out, buf, len);
}

static int fd_chr_read_poll(void *opaque)
{
    CharDriverState *chr = opaque;
    FDCharDriver *s = chr->opaque;

    s->max_size = qemu_chr_be_can_write(chr);
    return s->max_size;
}

static void fd_chr_read(void *opaque)
{
    CharDriverState *chr = opaque;
    FDCharDriver *s = chr->opaque;
    int size, len;
    uint8_t buf[READ_BUF_LEN];

    len = sizeof(buf);
    if (len > s->max_size)
        len = s->max_size;
    if (len == 0)
        return;
    size = read(s->fd_in, buf, len);
    if (size == 0) {
        /* FD has been closed. Remove it from the active list.  */
        qemu_set_fd_handler2(s->fd_in, NULL, NULL, NULL, NULL);
        qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
        return;
    }
    if (size > 0) {
        qemu_chr_be_write(chr, buf, size);
    }
}

static void fd_chr_update_read_handler(CharDriverState *chr)
{
    FDCharDriver *s = chr->opaque;

    if (s->fd_in >= 0) {
        qemu_set_fd_handler2(s->fd_in, fd_chr_read_poll,
                             fd_chr_read, NULL, chr);
    }
}

static void fd_chr_close(struct CharDriverState *chr)
{
    FDCharDriver *s = chr->opaque;

    if (s->fd_in >= 0) {
        qemu_set_fd_handler2(s->fd_in, NULL, NULL, NULL, NULL);
    }

    g_free(s);
    qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
}

/* open a character device to a unix fd */
static CharDriverState *qemu_chr_open_fd(int fd_in, int fd_out)
{
    CharDriverState *chr;
    FDCharDriver *s;

    chr = g_malloc0(sizeof(CharDriverState));
    s = g_malloc0(sizeof(FDCharDriver));
    s->fd_in = fd_in;
    s->fd_out = fd_out;
    chr->opaque = s;
    chr->chr_write = fd_chr_write;
    chr->chr_update_read_handler = fd_chr_update_read_handler;
    chr->chr_close = fd_chr_close;

    qemu_chr_generic_open(chr);

    return chr;
}

static CharDriverState *qemu_chr_open_file_out(QemuOpts *opts)
{
    int fd_out;

    TFR(fd_out = qemu_open(qemu_opt_get(opts, "path"),
                      O_WRONLY | O_TRUNC | O_CREAT | O_BINARY, 0666));
    if (fd_out < 0) {
        return NULL;
    }
    return qemu_chr_open_fd(-1, fd_out);
}

static CharDriverState *qemu_chr_open_pipe(QemuOpts *opts)
{
    int fd_in, fd_out;
    char filename_in[256], filename_out[256];
    const char *filename = qemu_opt_get(opts, "path");

    if (filename == NULL) {
        fprintf(stderr, "chardev: pipe: no filename given\n");
        return NULL;
    }

    snprintf(filename_in, 256, "%s.in", filename);
    snprintf(filename_out, 256, "%s.out", filename);
    TFR(fd_in = qemu_open(filename_in, O_RDWR | O_BINARY));
    TFR(fd_out = qemu_open(filename_out, O_RDWR | O_BINARY));
    if (fd_in < 0 || fd_out < 0) {
	if (fd_in >= 0)
	    close(fd_in);
	if (fd_out >= 0)
	    close(fd_out);
        TFR(fd_in = fd_out = qemu_open(filename, O_RDWR | O_BINARY));
        if (fd_in < 0) {
            return NULL;
        }
    }
    return qemu_chr_open_fd(fd_in, fd_out);
}

/* init terminal so that we can grab keys */
static struct termios oldtty;
static int old_fd0_flags;
static bool stdio_allow_signal;

static void term_exit(void)
{
    tcsetattr (0, TCSANOW, &oldtty);
    fcntl(0, F_SETFL, old_fd0_flags);
}

static void qemu_chr_set_echo_stdio(CharDriverState *chr, bool echo)
{
    struct termios tty;

    tty = oldtty;
    if (!echo) {
        tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                          |INLCR|IGNCR|ICRNL|IXON);
        tty.c_oflag |= OPOST;
        tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
        tty.c_cflag &= ~(CSIZE|PARENB);
        tty.c_cflag |= CS8;
        tty.c_cc[VMIN] = 1;
        tty.c_cc[VTIME] = 0;
    }
    /* if graphical mode, we allow Ctrl-C handling */
    if (!stdio_allow_signal)
        tty.c_lflag &= ~ISIG;

    tcsetattr (0, TCSANOW, &tty);
}

static void qemu_chr_close_stdio(struct CharDriverState *chr)
{
    term_exit();
    fd_chr_close(chr);
}

static CharDriverState *qemu_chr_open_stdio(QemuOpts *opts)
{
    CharDriverState *chr;

    if (is_daemonized()) {
        error_report("cannot use stdio with -daemonize");
        return NULL;
    }
    old_fd0_flags = fcntl(0, F_GETFL);
    tcgetattr (0, &oldtty);
    fcntl(0, F_SETFL, O_NONBLOCK);
    atexit(term_exit);

    chr = qemu_chr_open_fd(0, 1);
    chr->chr_close = qemu_chr_close_stdio;
    chr->chr_set_echo = qemu_chr_set_echo_stdio;
    stdio_allow_signal = qemu_opt_get_bool(opts, "signal",
                                           display_type != DT_NOGRAPHIC);
    qemu_chr_fe_set_echo(chr, false);

    return chr;
}

#ifdef __sun__
/* Once Solaris has openpty(), this is going to be removed. */
static int openpty(int *amaster, int *aslave, char *name,
                   struct termios *termp, struct winsize *winp)
{
        const char *slave;
        int mfd = -1, sfd = -1;

        *amaster = *aslave = -1;

        mfd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
        if (mfd < 0)
                goto err;

        if (grantpt(mfd) == -1 || unlockpt(mfd) == -1)
                goto err;

        if ((slave = ptsname(mfd)) == NULL)
                goto err;

        if ((sfd = open(slave, O_RDONLY | O_NOCTTY)) == -1)
                goto err;

        if (ioctl(sfd, I_PUSH, "ptem") == -1 ||
            (termp != NULL && tcgetattr(sfd, termp) < 0))
                goto err;

        if (amaster)
                *amaster = mfd;
        if (aslave)
                *aslave = sfd;
        if (winp)
                ioctl(sfd, TIOCSWINSZ, winp);

        return 0;

err:
        if (sfd != -1)
                close(sfd);
        close(mfd);
        return -1;
}

static void cfmakeraw (struct termios *termios_p)
{
        termios_p->c_iflag &=
                ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
        termios_p->c_oflag &= ~OPOST;
        termios_p->c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
        termios_p->c_cflag &= ~(CSIZE|PARENB);
        termios_p->c_cflag |= CS8;

        termios_p->c_cc[VMIN] = 0;
        termios_p->c_cc[VTIME] = 0;
}
#endif

#if defined(__linux__) || defined(__sun__) || defined(__FreeBSD__) \
    || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__) \
    || defined(__GLIBC__)

#define HAVE_CHARDEV_TTY 1

typedef struct {
    int fd;
    int connected;
    int polling;
    int read_bytes;
    QEMUTimer *timer;
} PtyCharDriver;

static void pty_chr_update_read_handler(CharDriverState *chr);
static void pty_chr_state(CharDriverState *chr, int connected);

static int pty_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    PtyCharDriver *s = chr->opaque;

    if (!s->connected) {
        /* guest sends data, check for (re-)connect */
        pty_chr_update_read_handler(chr);
        return 0;
    }
    return send_all(s->fd, buf, len);
}

static int pty_chr_read_poll(void *opaque)
{
    CharDriverState *chr = opaque;
    PtyCharDriver *s = chr->opaque;

    s->read_bytes = qemu_chr_be_can_write(chr);
    return s->read_bytes;
}

static void pty_chr_read(void *opaque)
{
    CharDriverState *chr = opaque;
    PtyCharDriver *s = chr->opaque;
    int size, len;
    uint8_t buf[READ_BUF_LEN];

    len = sizeof(buf);
    if (len > s->read_bytes)
        len = s->read_bytes;
    if (len == 0)
        return;
    size = read(s->fd, buf, len);
    if ((size == -1 && errno == EIO) ||
        (size == 0)) {
        pty_chr_state(chr, 0);
        return;
    }
    if (size > 0) {
        pty_chr_state(chr, 1);
        qemu_chr_be_write(chr, buf, size);
    }
}

static void pty_chr_update_read_handler(CharDriverState *chr)
{
    PtyCharDriver *s = chr->opaque;

    qemu_set_fd_handler2(s->fd, pty_chr_read_poll,
                         pty_chr_read, NULL, chr);
    s->polling = 1;
    /*
     * Short timeout here: just need wait long enougth that qemu makes
     * it through the poll loop once.  When reconnected we want a
     * short timeout so we notice it almost instantly.  Otherwise
     * read() gives us -EIO instantly, making pty_chr_state() reset the
     * timeout to the normal (much longer) poll interval before the
     * timer triggers.
     */
    qemu_mod_timer(s->timer, qemu_get_clock_ms(rt_clock) + 10);
}

static void pty_chr_state(CharDriverState *chr, int connected)
{
    PtyCharDriver *s = chr->opaque;

    if (!connected) {
        qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);
        s->connected = 0;
        s->polling = 0;
        /* (re-)connect poll interval for idle guests: once per second.
         * We check more frequently in case the guests sends data to
         * the virtual device linked to our pty. */
        qemu_mod_timer(s->timer, qemu_get_clock_ms(rt_clock) + 1000);
    } else {
        if (!s->connected)
            qemu_chr_generic_open(chr);
        s->connected = 1;
    }
}

static void pty_chr_timer(void *opaque)
{
    struct CharDriverState *chr = opaque;
    PtyCharDriver *s = chr->opaque;

    if (s->connected)
        return;
    if (s->polling) {
        /* If we arrive here without polling being cleared due
         * read returning -EIO, then we are (re-)connected */
        pty_chr_state(chr, 1);
        return;
    }

    /* Next poll ... */
    pty_chr_update_read_handler(chr);
}

static void pty_chr_close(struct CharDriverState *chr)
{
    PtyCharDriver *s = chr->opaque;

    qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);
    close(s->fd);
    qemu_del_timer(s->timer);
    qemu_free_timer(s->timer);
    g_free(s);
    qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
}

static CharDriverState *qemu_chr_open_pty(QemuOpts *opts)
{
    CharDriverState *chr;
    PtyCharDriver *s;
    struct termios tty;
    const char *label;
    int master_fd, slave_fd, len;
#if defined(__OpenBSD__) || defined(__DragonFly__)
    char pty_name[PATH_MAX];
#define q_ptsname(x) pty_name
#else
    char *pty_name = NULL;
#define q_ptsname(x) ptsname(x)
#endif

    if (openpty(&master_fd, &slave_fd, pty_name, NULL, NULL) < 0) {
        return NULL;
    }

    /* Set raw attributes on the pty. */
    tcgetattr(slave_fd, &tty);
    cfmakeraw(&tty);
    tcsetattr(slave_fd, TCSAFLUSH, &tty);
    close(slave_fd);

    chr = g_malloc0(sizeof(CharDriverState));

    len = strlen(q_ptsname(master_fd)) + 5;
    chr->filename = g_malloc(len);
    snprintf(chr->filename, len, "pty:%s", q_ptsname(master_fd));
    qemu_opt_set(opts, "path", q_ptsname(master_fd));

    label = qemu_opts_id(opts);
    fprintf(stderr, "char device redirected to %s%s%s%s\n",
            q_ptsname(master_fd),
            label ? " (label " : "",
            label ? label      : "",
            label ? ")"        : "");

    s = g_malloc0(sizeof(PtyCharDriver));
    chr->opaque = s;
    chr->chr_write = pty_chr_write;
    chr->chr_update_read_handler = pty_chr_update_read_handler;
    chr->chr_close = pty_chr_close;

    s->fd = master_fd;
    s->timer = qemu_new_timer_ms(rt_clock, pty_chr_timer, chr);

    return chr;
}

static void tty_serial_init(int fd, int speed,
                            int parity, int data_bits, int stop_bits)
{
    struct termios tty;
    speed_t spd;

#if 0
    printf("tty_serial_init: speed=%d parity=%c data=%d stop=%d\n",
           speed, parity, data_bits, stop_bits);
#endif
    tcgetattr (fd, &tty);

#define check_speed(val) if (speed <= val) { spd = B##val; break; }
    speed = speed * 10 / 11;
    do {
        check_speed(50);
        check_speed(75);
        check_speed(110);
        check_speed(134);
        check_speed(150);
        check_speed(200);
        check_speed(300);
        check_speed(600);
        check_speed(1200);
        check_speed(1800);
        check_speed(2400);
        check_speed(4800);
        check_speed(9600);
        check_speed(19200);
        check_speed(38400);
        /* Non-Posix values follow. They may be unsupported on some systems. */
        check_speed(57600);
        check_speed(115200);
#ifdef B230400
        check_speed(230400);
#endif
#ifdef B460800
        check_speed(460800);
#endif
#ifdef B500000
        check_speed(500000);
#endif
#ifdef B576000
        check_speed(576000);
#endif
#ifdef B921600
        check_speed(921600);
#endif
#ifdef B1000000
        check_speed(1000000);
#endif
#ifdef B1152000
        check_speed(1152000);
#endif
#ifdef B1500000
        check_speed(1500000);
#endif
#ifdef B2000000
        check_speed(2000000);
#endif
#ifdef B2500000
        check_speed(2500000);
#endif
#ifdef B3000000
        check_speed(3000000);
#endif
#ifdef B3500000
        check_speed(3500000);
#endif
#ifdef B4000000
        check_speed(4000000);
#endif
        spd = B115200;
    } while (0);

    cfsetispeed(&tty, spd);
    cfsetospeed(&tty, spd);

    tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                          |INLCR|IGNCR|ICRNL|IXON);
    tty.c_oflag |= OPOST;
    tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN|ISIG);
    tty.c_cflag &= ~(CSIZE|PARENB|PARODD|CRTSCTS|CSTOPB);
    switch(data_bits) {
    default:
    case 8:
        tty.c_cflag |= CS8;
        break;
    case 7:
        tty.c_cflag |= CS7;
        break;
    case 6:
        tty.c_cflag |= CS6;
        break;
    case 5:
        tty.c_cflag |= CS5;
        break;
    }
    switch(parity) {
    default:
    case 'N':
        break;
    case 'E':
        tty.c_cflag |= PARENB;
        break;
    case 'O':
        tty.c_cflag |= PARENB | PARODD;
        break;
    }
    if (stop_bits == 2)
        tty.c_cflag |= CSTOPB;

    tcsetattr (fd, TCSANOW, &tty);
}

static int tty_serial_ioctl(CharDriverState *chr, int cmd, void *arg)
{
    FDCharDriver *s = chr->opaque;

    switch(cmd) {
    case CHR_IOCTL_SERIAL_SET_PARAMS:
        {
            QEMUSerialSetParams *ssp = arg;
            tty_serial_init(s->fd_in, ssp->speed, ssp->parity,
                            ssp->data_bits, ssp->stop_bits);
        }
        break;
    case CHR_IOCTL_SERIAL_SET_BREAK:
        {
            int enable = *(int *)arg;
            if (enable)
                tcsendbreak(s->fd_in, 1);
        }
        break;
    case CHR_IOCTL_SERIAL_GET_TIOCM:
        {
            int sarg = 0;
            int *targ = (int *)arg;
            ioctl(s->fd_in, TIOCMGET, &sarg);
            *targ = 0;
            if (sarg & TIOCM_CTS)
                *targ |= CHR_TIOCM_CTS;
            if (sarg & TIOCM_CAR)
                *targ |= CHR_TIOCM_CAR;
            if (sarg & TIOCM_DSR)
                *targ |= CHR_TIOCM_DSR;
            if (sarg & TIOCM_RI)
                *targ |= CHR_TIOCM_RI;
            if (sarg & TIOCM_DTR)
                *targ |= CHR_TIOCM_DTR;
            if (sarg & TIOCM_RTS)
                *targ |= CHR_TIOCM_RTS;
        }
        break;
    case CHR_IOCTL_SERIAL_SET_TIOCM:
        {
            int sarg = *(int *)arg;
            int targ = 0;
            ioctl(s->fd_in, TIOCMGET, &targ);
            targ &= ~(CHR_TIOCM_CTS | CHR_TIOCM_CAR | CHR_TIOCM_DSR
                     | CHR_TIOCM_RI | CHR_TIOCM_DTR | CHR_TIOCM_RTS);
            if (sarg & CHR_TIOCM_CTS)
                targ |= TIOCM_CTS;
            if (sarg & CHR_TIOCM_CAR)
                targ |= TIOCM_CAR;
            if (sarg & CHR_TIOCM_DSR)
                targ |= TIOCM_DSR;
            if (sarg & CHR_TIOCM_RI)
                targ |= TIOCM_RI;
            if (sarg & CHR_TIOCM_DTR)
                targ |= TIOCM_DTR;
            if (sarg & CHR_TIOCM_RTS)
                targ |= TIOCM_RTS;
            ioctl(s->fd_in, TIOCMSET, &targ);
        }
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
}

static void qemu_chr_close_tty(CharDriverState *chr)
{
    FDCharDriver *s = chr->opaque;
    int fd = -1;

    if (s) {
        fd = s->fd_in;
    }

    fd_chr_close(chr);

    if (fd >= 0) {
        close(fd);
    }
}

static CharDriverState *qemu_chr_open_tty_fd(int fd)
{
    CharDriverState *chr;

    tty_serial_init(fd, 115200, 'N', 8, 1);
    chr = qemu_chr_open_fd(fd, fd);
    chr->chr_ioctl = tty_serial_ioctl;
    chr->chr_close = qemu_chr_close_tty;
    return chr;
}

static CharDriverState *qemu_chr_open_tty(QemuOpts *opts)
{
    const char *filename = qemu_opt_get(opts, "path");
    int fd;

    TFR(fd = qemu_open(filename, O_RDWR | O_NONBLOCK));
    if (fd < 0) {
        return NULL;
    }
    return qemu_chr_open_tty_fd(fd);
}
#endif /* __linux__ || __sun__ */

#if defined(__linux__)

#define HAVE_CHARDEV_PARPORT 1

typedef struct {
    int fd;
    int mode;
} ParallelCharDriver;

static int pp_hw_mode(ParallelCharDriver *s, uint16_t mode)
{
    if (s->mode != mode) {
	int m = mode;
        if (ioctl(s->fd, PPSETMODE, &m) < 0)
            return 0;
	s->mode = mode;
    }
    return 1;
}

static int pp_ioctl(CharDriverState *chr, int cmd, void *arg)
{
    ParallelCharDriver *drv = chr->opaque;
    int fd = drv->fd;
    uint8_t b;

    switch(cmd) {
    case CHR_IOCTL_PP_READ_DATA:
        if (ioctl(fd, PPRDATA, &b) < 0)
            return -ENOTSUP;
        *(uint8_t *)arg = b;
        break;
    case CHR_IOCTL_PP_WRITE_DATA:
        b = *(uint8_t *)arg;
        if (ioctl(fd, PPWDATA, &b) < 0)
            return -ENOTSUP;
        break;
    case CHR_IOCTL_PP_READ_CONTROL:
        if (ioctl(fd, PPRCONTROL, &b) < 0)
            return -ENOTSUP;
	/* Linux gives only the lowest bits, and no way to know data
	   direction! For better compatibility set the fixed upper
	   bits. */
        *(uint8_t *)arg = b | 0xc0;
        break;
    case CHR_IOCTL_PP_WRITE_CONTROL:
        b = *(uint8_t *)arg;
        if (ioctl(fd, PPWCONTROL, &b) < 0)
            return -ENOTSUP;
        break;
    case CHR_IOCTL_PP_READ_STATUS:
        if (ioctl(fd, PPRSTATUS, &b) < 0)
            return -ENOTSUP;
        *(uint8_t *)arg = b;
        break;
    case CHR_IOCTL_PP_DATA_DIR:
        if (ioctl(fd, PPDATADIR, (int *)arg) < 0)
            return -ENOTSUP;
        break;
    case CHR_IOCTL_PP_EPP_READ_ADDR:
	if (pp_hw_mode(drv, IEEE1284_MODE_EPP|IEEE1284_ADDR)) {
	    struct ParallelIOArg *parg = arg;
	    int n = read(fd, parg->buffer, parg->count);
	    if (n != parg->count) {
		return -EIO;
	    }
	}
        break;
    case CHR_IOCTL_PP_EPP_READ:
	if (pp_hw_mode(drv, IEEE1284_MODE_EPP)) {
	    struct ParallelIOArg *parg = arg;
	    int n = read(fd, parg->buffer, parg->count);
	    if (n != parg->count) {
		return -EIO;
	    }
	}
        break;
    case CHR_IOCTL_PP_EPP_WRITE_ADDR:
	if (pp_hw_mode(drv, IEEE1284_MODE_EPP|IEEE1284_ADDR)) {
	    struct ParallelIOArg *parg = arg;
	    int n = write(fd, parg->buffer, parg->count);
	    if (n != parg->count) {
		return -EIO;
	    }
	}
        break;
    case CHR_IOCTL_PP_EPP_WRITE:
	if (pp_hw_mode(drv, IEEE1284_MODE_EPP)) {
	    struct ParallelIOArg *parg = arg;
	    int n = write(fd, parg->buffer, parg->count);
	    if (n != parg->count) {
		return -EIO;
	    }
	}
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
}

static void pp_close(CharDriverState *chr)
{
    ParallelCharDriver *drv = chr->opaque;
    int fd = drv->fd;

    pp_hw_mode(drv, IEEE1284_MODE_COMPAT);
    ioctl(fd, PPRELEASE);
    close(fd);
    g_free(drv);
    qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
}

static CharDriverState *qemu_chr_open_pp_fd(int fd)
{
    CharDriverState *chr;
    ParallelCharDriver *drv;

    if (ioctl(fd, PPCLAIM) < 0) {
        close(fd);
        return NULL;
    }

    drv = g_malloc0(sizeof(ParallelCharDriver));
    drv->fd = fd;
    drv->mode = IEEE1284_MODE_COMPAT;

    chr = g_malloc0(sizeof(CharDriverState));
    chr->chr_write = null_chr_write;
    chr->chr_ioctl = pp_ioctl;
    chr->chr_close = pp_close;
    chr->opaque = drv;

    qemu_chr_generic_open(chr);

    return chr;
}
#endif /* __linux__ */

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)

#define HAVE_CHARDEV_PARPORT 1

static int pp_ioctl(CharDriverState *chr, int cmd, void *arg)
{
    int fd = (int)(intptr_t)chr->opaque;
    uint8_t b;

    switch(cmd) {
    case CHR_IOCTL_PP_READ_DATA:
        if (ioctl(fd, PPIGDATA, &b) < 0)
            return -ENOTSUP;
        *(uint8_t *)arg = b;
        break;
    case CHR_IOCTL_PP_WRITE_DATA:
        b = *(uint8_t *)arg;
        if (ioctl(fd, PPISDATA, &b) < 0)
            return -ENOTSUP;
        break;
    case CHR_IOCTL_PP_READ_CONTROL:
        if (ioctl(fd, PPIGCTRL, &b) < 0)
            return -ENOTSUP;
        *(uint8_t *)arg = b;
        break;
    case CHR_IOCTL_PP_WRITE_CONTROL:
        b = *(uint8_t *)arg;
        if (ioctl(fd, PPISCTRL, &b) < 0)
            return -ENOTSUP;
        break;
    case CHR_IOCTL_PP_READ_STATUS:
        if (ioctl(fd, PPIGSTATUS, &b) < 0)
            return -ENOTSUP;
        *(uint8_t *)arg = b;
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
}

static CharDriverState *qemu_chr_open_pp_fd(int fd)
{
    CharDriverState *chr;

    chr = g_malloc0(sizeof(CharDriverState));
    chr->opaque = (void *)(intptr_t)fd;
    chr->chr_write = null_chr_write;
    chr->chr_ioctl = pp_ioctl;
    return chr;
}
#endif

#else /* _WIN32 */

typedef struct {
    int max_size;
    HANDLE hcom, hrecv, hsend;
    OVERLAPPED orecv, osend;
    BOOL fpipe;
    DWORD len;
} WinCharState;

typedef struct {
    HANDLE  hStdIn;
    HANDLE  hInputReadyEvent;
    HANDLE  hInputDoneEvent;
    HANDLE  hInputThread;
    uint8_t win_stdio_buf;
} WinStdioCharState;

#define NSENDBUF 2048
#define NRECVBUF 2048
#define MAXCONNECT 1
#define NTIMEOUT 5000

static int win_chr_poll(void *opaque);
static int win_chr_pipe_poll(void *opaque);

static void win_chr_close(CharDriverState *chr)
{
    WinCharState *s = chr->opaque;

    if (s->hsend) {
        CloseHandle(s->hsend);
        s->hsend = NULL;
    }
    if (s->hrecv) {
        CloseHandle(s->hrecv);
        s->hrecv = NULL;
    }
    if (s->hcom) {
        CloseHandle(s->hcom);
        s->hcom = NULL;
    }
    if (s->fpipe)
        qemu_del_polling_cb(win_chr_pipe_poll, chr);
    else
        qemu_del_polling_cb(win_chr_poll, chr);

    qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
}

static int win_chr_init(CharDriverState *chr, const char *filename)
{
    WinCharState *s = chr->opaque;
    COMMCONFIG comcfg;
    COMMTIMEOUTS cto = { 0, 0, 0, 0, 0};
    COMSTAT comstat;
    DWORD size;
    DWORD err;

    s->hsend = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!s->hsend) {
        fprintf(stderr, "Failed CreateEvent\n");
        goto fail;
    }
    s->hrecv = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!s->hrecv) {
        fprintf(stderr, "Failed CreateEvent\n");
        goto fail;
    }

    s->hcom = CreateFile(filename, GENERIC_READ|GENERIC_WRITE, 0, NULL,
                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    if (s->hcom == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed CreateFile (%lu)\n", GetLastError());
        s->hcom = NULL;
        goto fail;
    }

    if (!SetupComm(s->hcom, NRECVBUF, NSENDBUF)) {
        fprintf(stderr, "Failed SetupComm\n");
        goto fail;
    }

    ZeroMemory(&comcfg, sizeof(COMMCONFIG));
    size = sizeof(COMMCONFIG);
    GetDefaultCommConfig(filename, &comcfg, &size);
    comcfg.dcb.DCBlength = sizeof(DCB);
    CommConfigDialog(filename, NULL, &comcfg);

    if (!SetCommState(s->hcom, &comcfg.dcb)) {
        fprintf(stderr, "Failed SetCommState\n");
        goto fail;
    }

    if (!SetCommMask(s->hcom, EV_ERR)) {
        fprintf(stderr, "Failed SetCommMask\n");
        goto fail;
    }

    cto.ReadIntervalTimeout = MAXDWORD;
    if (!SetCommTimeouts(s->hcom, &cto)) {
        fprintf(stderr, "Failed SetCommTimeouts\n");
        goto fail;
    }

    if (!ClearCommError(s->hcom, &err, &comstat)) {
        fprintf(stderr, "Failed ClearCommError\n");
        goto fail;
    }
    qemu_add_polling_cb(win_chr_poll, chr);
    return 0;

 fail:
    win_chr_close(chr);
    return -1;
}

static int win_chr_write(CharDriverState *chr, const uint8_t *buf, int len1)
{
    WinCharState *s = chr->opaque;
    DWORD len, ret, size, err;

    len = len1;
    ZeroMemory(&s->osend, sizeof(s->osend));
    s->osend.hEvent = s->hsend;
    while (len > 0) {
        if (s->hsend)
            ret = WriteFile(s->hcom, buf, len, &size, &s->osend);
        else
            ret = WriteFile(s->hcom, buf, len, &size, NULL);
        if (!ret) {
            err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                ret = GetOverlappedResult(s->hcom, &s->osend, &size, TRUE);
                if (ret) {
                    buf += size;
                    len -= size;
                } else {
                    break;
                }
            } else {
                break;
            }
        } else {
            buf += size;
            len -= size;
        }
    }
    return len1 - len;
}

static int win_chr_read_poll(CharDriverState *chr)
{
    WinCharState *s = chr->opaque;

    s->max_size = qemu_chr_be_can_write(chr);
    return s->max_size;
}

static void win_chr_readfile(CharDriverState *chr)
{
    WinCharState *s = chr->opaque;
    int ret, err;
    uint8_t buf[READ_BUF_LEN];
    DWORD size;

    ZeroMemory(&s->orecv, sizeof(s->orecv));
    s->orecv.hEvent = s->hrecv;
    ret = ReadFile(s->hcom, buf, s->len, &size, &s->orecv);
    if (!ret) {
        err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            ret = GetOverlappedResult(s->hcom, &s->orecv, &size, TRUE);
        }
    }

    if (size > 0) {
        qemu_chr_be_write(chr, buf, size);
    }
}

static void win_chr_read(CharDriverState *chr)
{
    WinCharState *s = chr->opaque;

    if (s->len > s->max_size)
        s->len = s->max_size;
    if (s->len == 0)
        return;

    win_chr_readfile(chr);
}

static int win_chr_poll(void *opaque)
{
    CharDriverState *chr = opaque;
    WinCharState *s = chr->opaque;
    COMSTAT status;
    DWORD comerr;

    ClearCommError(s->hcom, &comerr, &status);
    if (status.cbInQue > 0) {
        s->len = status.cbInQue;
        win_chr_read_poll(chr);
        win_chr_read(chr);
        return 1;
    }
    return 0;
}

static CharDriverState *qemu_chr_open_win_path(const char *filename)
{
    CharDriverState *chr;
    WinCharState *s;

    chr = g_malloc0(sizeof(CharDriverState));
    s = g_malloc0(sizeof(WinCharState));
    chr->opaque = s;
    chr->chr_write = win_chr_write;
    chr->chr_close = win_chr_close;

    if (win_chr_init(chr, filename) < 0) {
        g_free(s);
        g_free(chr);
        return NULL;
    }
    qemu_chr_generic_open(chr);
    return chr;
}

static CharDriverState *qemu_chr_open_win(QemuOpts *opts)
{
    return qemu_chr_open_win_path(qemu_opt_get(opts, "path"));
}

static int win_chr_pipe_poll(void *opaque)
{
    CharDriverState *chr = opaque;
    WinCharState *s = chr->opaque;
    DWORD size;

    PeekNamedPipe(s->hcom, NULL, 0, NULL, &size, NULL);
    if (size > 0) {
        s->len = size;
        win_chr_read_poll(chr);
        win_chr_read(chr);
        return 1;
    }
    return 0;
}

static int win_chr_pipe_init(CharDriverState *chr, const char *filename)
{
    WinCharState *s = chr->opaque;
    OVERLAPPED ov;
    int ret;
    DWORD size;
    char openname[256];

    s->fpipe = TRUE;

    s->hsend = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!s->hsend) {
        fprintf(stderr, "Failed CreateEvent\n");
        goto fail;
    }
    s->hrecv = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!s->hrecv) {
        fprintf(stderr, "Failed CreateEvent\n");
        goto fail;
    }

    snprintf(openname, sizeof(openname), "\\\\.\\pipe\\%s", filename);
    s->hcom = CreateNamedPipe(openname, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                              PIPE_WAIT,
                              MAXCONNECT, NSENDBUF, NRECVBUF, NTIMEOUT, NULL);
    if (s->hcom == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed CreateNamedPipe (%lu)\n", GetLastError());
        s->hcom = NULL;
        goto fail;
    }

    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ret = ConnectNamedPipe(s->hcom, &ov);
    if (ret) {
        fprintf(stderr, "Failed ConnectNamedPipe\n");
        goto fail;
    }

    ret = GetOverlappedResult(s->hcom, &ov, &size, TRUE);
    if (!ret) {
        fprintf(stderr, "Failed GetOverlappedResult\n");
        if (ov.hEvent) {
            CloseHandle(ov.hEvent);
            ov.hEvent = NULL;
        }
        goto fail;
    }

    if (ov.hEvent) {
        CloseHandle(ov.hEvent);
        ov.hEvent = NULL;
    }
    qemu_add_polling_cb(win_chr_pipe_poll, chr);
    return 0;

 fail:
    win_chr_close(chr);
    return -1;
}


static CharDriverState *qemu_chr_open_win_pipe(QemuOpts *opts)
{
    const char *filename = qemu_opt_get(opts, "path");
    CharDriverState *chr;
    WinCharState *s;

    chr = g_malloc0(sizeof(CharDriverState));
    s = g_malloc0(sizeof(WinCharState));
    chr->opaque = s;
    chr->chr_write = win_chr_write;
    chr->chr_close = win_chr_close;

    if (win_chr_pipe_init(chr, filename) < 0) {
        g_free(s);
        g_free(chr);
        return NULL;
    }
    qemu_chr_generic_open(chr);
    return chr;
}

static CharDriverState *qemu_chr_open_win_file(HANDLE fd_out)
{
    CharDriverState *chr;
    WinCharState *s;

    chr = g_malloc0(sizeof(CharDriverState));
    s = g_malloc0(sizeof(WinCharState));
    s->hcom = fd_out;
    chr->opaque = s;
    chr->chr_write = win_chr_write;
    qemu_chr_generic_open(chr);
    return chr;
}

static CharDriverState *qemu_chr_open_win_con(QemuOpts *opts)
{
    return qemu_chr_open_win_file(GetStdHandle(STD_OUTPUT_HANDLE));
}

static CharDriverState *qemu_chr_open_win_file_out(QemuOpts *opts)
{
    const char *file_out = qemu_opt_get(opts, "path");
    HANDLE fd_out;

    fd_out = CreateFile(file_out, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fd_out == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    return qemu_chr_open_win_file(fd_out);
}

static int win_stdio_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    HANDLE  hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD   dwSize;
    int     len1;

    len1 = len;

    while (len1 > 0) {
        if (!WriteFile(hStdOut, buf, len1, &dwSize, NULL)) {
            break;
        }
        buf  += dwSize;
        len1 -= dwSize;
    }

    return len - len1;
}

static void win_stdio_wait_func(void *opaque)
{
    CharDriverState   *chr   = opaque;
    WinStdioCharState *stdio = chr->opaque;
    INPUT_RECORD       buf[4];
    int                ret;
    DWORD              dwSize;
    int                i;

    ret = ReadConsoleInput(stdio->hStdIn, buf, sizeof(buf) / sizeof(*buf),
                           &dwSize);

    if (!ret) {
        /* Avoid error storm */
        qemu_del_wait_object(stdio->hStdIn, NULL, NULL);
        return;
    }

    for (i = 0; i < dwSize; i++) {
        KEY_EVENT_RECORD *kev = &buf[i].Event.KeyEvent;

        if (buf[i].EventType == KEY_EVENT && kev->bKeyDown) {
            int j;
            if (kev->uChar.AsciiChar != 0) {
                for (j = 0; j < kev->wRepeatCount; j++) {
                    if (qemu_chr_be_can_write(chr)) {
                        uint8_t c = kev->uChar.AsciiChar;
                        qemu_chr_be_write(chr, &c, 1);
                    }
                }
            }
        }
    }
}

static DWORD WINAPI win_stdio_thread(LPVOID param)
{
    CharDriverState   *chr   = param;
    WinStdioCharState *stdio = chr->opaque;
    int                ret;
    DWORD              dwSize;

    while (1) {

        /* Wait for one byte */
        ret = ReadFile(stdio->hStdIn, &stdio->win_stdio_buf, 1, &dwSize, NULL);

        /* Exit in case of error, continue if nothing read */
        if (!ret) {
            break;
        }
        if (!dwSize) {
            continue;
        }

        /* Some terminal emulator returns \r\n for Enter, just pass \n */
        if (stdio->win_stdio_buf == '\r') {
            continue;
        }

        /* Signal the main thread and wait until the byte was eaten */
        if (!SetEvent(stdio->hInputReadyEvent)) {
            break;
        }
        if (WaitForSingleObject(stdio->hInputDoneEvent, INFINITE)
            != WAIT_OBJECT_0) {
            break;
        }
    }

    qemu_del_wait_object(stdio->hInputReadyEvent, NULL, NULL);
    return 0;
}

static void win_stdio_thread_wait_func(void *opaque)
{
    CharDriverState   *chr   = opaque;
    WinStdioCharState *stdio = chr->opaque;

    if (qemu_chr_be_can_write(chr)) {
        qemu_chr_be_write(chr, &stdio->win_stdio_buf, 1);
    }

    SetEvent(stdio->hInputDoneEvent);
}

static void qemu_chr_set_echo_win_stdio(CharDriverState *chr, bool echo)
{
    WinStdioCharState *stdio  = chr->opaque;
    DWORD              dwMode = 0;

    GetConsoleMode(stdio->hStdIn, &dwMode);

    if (echo) {
        SetConsoleMode(stdio->hStdIn, dwMode | ENABLE_ECHO_INPUT);
    } else {
        SetConsoleMode(stdio->hStdIn, dwMode & ~ENABLE_ECHO_INPUT);
    }
}

static void win_stdio_close(CharDriverState *chr)
{
    WinStdioCharState *stdio = chr->opaque;

    if (stdio->hInputReadyEvent != INVALID_HANDLE_VALUE) {
        CloseHandle(stdio->hInputReadyEvent);
    }
    if (stdio->hInputDoneEvent != INVALID_HANDLE_VALUE) {
        CloseHandle(stdio->hInputDoneEvent);
    }
    if (stdio->hInputThread != INVALID_HANDLE_VALUE) {
        TerminateThread(stdio->hInputThread, 0);
    }

    g_free(chr->opaque);
    g_free(chr);
}

static CharDriverState *qemu_chr_open_win_stdio(QemuOpts *opts)
{
    CharDriverState   *chr;
    WinStdioCharState *stdio;
    DWORD              dwMode;
    int                is_console = 0;

    chr   = g_malloc0(sizeof(CharDriverState));
    stdio = g_malloc0(sizeof(WinStdioCharState));

    stdio->hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (stdio->hStdIn == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot open stdio: invalid handle\n");
        exit(1);
    }

    is_console = GetConsoleMode(stdio->hStdIn, &dwMode) != 0;

    chr->opaque    = stdio;
    chr->chr_write = win_stdio_write;
    chr->chr_close = win_stdio_close;

    if (is_console) {
        if (qemu_add_wait_object(stdio->hStdIn,
                                 win_stdio_wait_func, chr)) {
            fprintf(stderr, "qemu_add_wait_object: failed\n");
        }
    } else {
        DWORD   dwId;
            
        stdio->hInputReadyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        stdio->hInputDoneEvent  = CreateEvent(NULL, FALSE, FALSE, NULL);
        stdio->hInputThread     = CreateThread(NULL, 0, win_stdio_thread,
                                               chr, 0, &dwId);

        if (stdio->hInputThread == INVALID_HANDLE_VALUE
            || stdio->hInputReadyEvent == INVALID_HANDLE_VALUE
            || stdio->hInputDoneEvent == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "cannot create stdio thread or event\n");
            exit(1);
        }
        if (qemu_add_wait_object(stdio->hInputReadyEvent,
                                 win_stdio_thread_wait_func, chr)) {
            fprintf(stderr, "qemu_add_wait_object: failed\n");
        }
    }

    dwMode |= ENABLE_LINE_INPUT;

    if (is_console) {
        /* set the terminal in raw mode */
        /* ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS */
        dwMode |= ENABLE_PROCESSED_INPUT;
    }

    SetConsoleMode(stdio->hStdIn, dwMode);

    chr->chr_set_echo = qemu_chr_set_echo_win_stdio;
    qemu_chr_fe_set_echo(chr, false);

    return chr;
}
#endif /* !_WIN32 */

/***********************************************************/
/* UDP Net console */

typedef struct {
    int fd;
    uint8_t buf[READ_BUF_LEN];
    int bufcnt;
    int bufptr;
    int max_size;
} NetCharDriver;

static int udp_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    NetCharDriver *s = chr->opaque;

    return send(s->fd, (const void *)buf, len, 0);
}

static int udp_chr_read_poll(void *opaque)
{
    CharDriverState *chr = opaque;
    NetCharDriver *s = chr->opaque;

    s->max_size = qemu_chr_be_can_write(chr);

    /* If there were any stray characters in the queue process them
     * first
     */
    while (s->max_size > 0 && s->bufptr < s->bufcnt) {
        qemu_chr_be_write(chr, &s->buf[s->bufptr], 1);
        s->bufptr++;
        s->max_size = qemu_chr_be_can_write(chr);
    }
    return s->max_size;
}

static void udp_chr_read(void *opaque)
{
    CharDriverState *chr = opaque;
    NetCharDriver *s = chr->opaque;

    if (s->max_size == 0)
        return;
    s->bufcnt = qemu_recv(s->fd, s->buf, sizeof(s->buf), 0);
    s->bufptr = s->bufcnt;
    if (s->bufcnt <= 0)
        return;

    s->bufptr = 0;
    while (s->max_size > 0 && s->bufptr < s->bufcnt) {
        qemu_chr_be_write(chr, &s->buf[s->bufptr], 1);
        s->bufptr++;
        s->max_size = qemu_chr_be_can_write(chr);
    }
}

static void udp_chr_update_read_handler(CharDriverState *chr)
{
    NetCharDriver *s = chr->opaque;

    if (s->fd >= 0) {
        qemu_set_fd_handler2(s->fd, udp_chr_read_poll,
                             udp_chr_read, NULL, chr);
    }
}

static void udp_chr_close(CharDriverState *chr)
{
    NetCharDriver *s = chr->opaque;
    if (s->fd >= 0) {
        qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);
        closesocket(s->fd);
    }
    g_free(s);
    qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
}

static CharDriverState *qemu_chr_open_udp(QemuOpts *opts)
{
    CharDriverState *chr = NULL;
    NetCharDriver *s = NULL;
    Error *local_err = NULL;
    int fd = -1;

    chr = g_malloc0(sizeof(CharDriverState));
    s = g_malloc0(sizeof(NetCharDriver));

    fd = inet_dgram_opts(opts, &local_err);
    if (fd < 0) {
        goto return_err;
    }

    s->fd = fd;
    s->bufcnt = 0;
    s->bufptr = 0;
    chr->opaque = s;
    chr->chr_write = udp_chr_write;
    chr->chr_update_read_handler = udp_chr_update_read_handler;
    chr->chr_close = udp_chr_close;
    return chr;

return_err:
    if (local_err) {
        qerror_report_err(local_err);
        error_free(local_err);
    }
    g_free(chr);
    g_free(s);
    if (fd >= 0) {
        closesocket(fd);
    }
    return NULL;
}

/***********************************************************/
/* TCP Net console */

typedef struct {
    int fd, listen_fd;
    int connected;
    int max_size;
    int do_telnetopt;
    int do_nodelay;
    int is_unix;
    int msgfd;
} TCPCharDriver;

static void tcp_chr_accept(void *opaque);

static int tcp_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    TCPCharDriver *s = chr->opaque;
    if (s->connected) {
        return send_all(s->fd, buf, len);
    } else {
        /* XXX: indicate an error ? */
        return len;
    }
}

static int tcp_chr_read_poll(void *opaque)
{
    CharDriverState *chr = opaque;
    TCPCharDriver *s = chr->opaque;
    if (!s->connected)
        return 0;
    s->max_size = qemu_chr_be_can_write(chr);
    return s->max_size;
}

#define IAC 255
#define IAC_BREAK 243
static void tcp_chr_process_IAC_bytes(CharDriverState *chr,
                                      TCPCharDriver *s,
                                      uint8_t *buf, int *size)
{
    /* Handle any telnet client's basic IAC options to satisfy char by
     * char mode with no echo.  All IAC options will be removed from
     * the buf and the do_telnetopt variable will be used to track the
     * state of the width of the IAC information.
     *
     * IAC commands come in sets of 3 bytes with the exception of the
     * "IAC BREAK" command and the double IAC.
     */

    int i;
    int j = 0;

    for (i = 0; i < *size; i++) {
        if (s->do_telnetopt > 1) {
            if ((unsigned char)buf[i] == IAC && s->do_telnetopt == 2) {
                /* Double IAC means send an IAC */
                if (j != i)
                    buf[j] = buf[i];
                j++;
                s->do_telnetopt = 1;
            } else {
                if ((unsigned char)buf[i] == IAC_BREAK && s->do_telnetopt == 2) {
                    /* Handle IAC break commands by sending a serial break */
                    qemu_chr_be_event(chr, CHR_EVENT_BREAK);
                    s->do_telnetopt++;
                }
                s->do_telnetopt++;
            }
            if (s->do_telnetopt >= 4) {
                s->do_telnetopt = 1;
            }
        } else {
            if ((unsigned char)buf[i] == IAC) {
                s->do_telnetopt = 2;
            } else {
                if (j != i)
                    buf[j] = buf[i];
                j++;
            }
        }
    }
    *size = j;
}

static int tcp_get_msgfd(CharDriverState *chr)
{
    TCPCharDriver *s = chr->opaque;
    int fd = s->msgfd;
    s->msgfd = -1;
    return fd;
}

#ifndef _WIN32
static void unix_process_msgfd(CharDriverState *chr, struct msghdr *msg)
{
    TCPCharDriver *s = chr->opaque;
    struct cmsghdr *cmsg;

    for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        int fd;

        if (cmsg->cmsg_len != CMSG_LEN(sizeof(int)) ||
            cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS)
            continue;

        fd = *((int *)CMSG_DATA(cmsg));
        if (fd < 0)
            continue;

#ifndef MSG_CMSG_CLOEXEC
        qemu_set_cloexec(fd);
#endif
        if (s->msgfd != -1)
            close(s->msgfd);
        s->msgfd = fd;
    }
}

static ssize_t tcp_chr_recv(CharDriverState *chr, char *buf, size_t len)
{
    TCPCharDriver *s = chr->opaque;
    struct msghdr msg = { NULL, };
    struct iovec iov[1];
    union {
        struct cmsghdr cmsg;
        char control[CMSG_SPACE(sizeof(int))];
    } msg_control;
    int flags = 0;
    ssize_t ret;

    iov[0].iov_base = buf;
    iov[0].iov_len = len;

    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = &msg_control;
    msg.msg_controllen = sizeof(msg_control);

#ifdef MSG_CMSG_CLOEXEC
    flags |= MSG_CMSG_CLOEXEC;
#endif
    ret = recvmsg(s->fd, &msg, flags);
    if (ret > 0 && s->is_unix) {
        unix_process_msgfd(chr, &msg);
    }

    return ret;
}
#else
static ssize_t tcp_chr_recv(CharDriverState *chr, char *buf, size_t len)
{
    TCPCharDriver *s = chr->opaque;
    return qemu_recv(s->fd, buf, len, 0);
}
#endif

static void tcp_chr_read(void *opaque)
{
    CharDriverState *chr = opaque;
    TCPCharDriver *s = chr->opaque;
    uint8_t buf[READ_BUF_LEN];
    int len, size;

    if (!s->connected || s->max_size <= 0)
        return;
    len = sizeof(buf);
    if (len > s->max_size)
        len = s->max_size;
    size = tcp_chr_recv(chr, (void *)buf, len);
    if (size == 0) {
        /* connection closed */
        s->connected = 0;
        if (s->listen_fd >= 0) {
            qemu_set_fd_handler2(s->listen_fd, NULL, tcp_chr_accept, NULL, chr);
        }
        qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);
        closesocket(s->fd);
        s->fd = -1;
        qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
    } else if (size > 0) {
        if (s->do_telnetopt)
            tcp_chr_process_IAC_bytes(chr, s, buf, &size);
        if (size > 0)
            qemu_chr_be_write(chr, buf, size);
    }
}

#ifndef _WIN32
CharDriverState *qemu_chr_open_eventfd(int eventfd)
{
    return qemu_chr_open_fd(eventfd, eventfd);
}
#endif

static void tcp_chr_connect(void *opaque)
{
    CharDriverState *chr = opaque;
    TCPCharDriver *s = chr->opaque;

    s->connected = 1;
    if (s->fd >= 0) {
        qemu_set_fd_handler2(s->fd, tcp_chr_read_poll,
                             tcp_chr_read, NULL, chr);
    }
    qemu_chr_generic_open(chr);
}

#define IACSET(x,a,b,c) x[0] = a; x[1] = b; x[2] = c;
static void tcp_chr_telnet_init(int fd)
{
    char buf[3];
    /* Send the telnet negotion to put telnet in binary, no echo, single char mode */
    IACSET(buf, 0xff, 0xfb, 0x01);  /* IAC WILL ECHO */
    send(fd, (char *)buf, 3, 0);
    IACSET(buf, 0xff, 0xfb, 0x03);  /* IAC WILL Suppress go ahead */
    send(fd, (char *)buf, 3, 0);
    IACSET(buf, 0xff, 0xfb, 0x00);  /* IAC WILL Binary */
    send(fd, (char *)buf, 3, 0);
    IACSET(buf, 0xff, 0xfd, 0x00);  /* IAC DO Binary */
    send(fd, (char *)buf, 3, 0);
}

static void socket_set_nodelay(int fd)
{
    int val = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&val, sizeof(val));
}

static int tcp_chr_add_client(CharDriverState *chr, int fd)
{
    TCPCharDriver *s = chr->opaque;
    if (s->fd != -1)
	return -1;

    socket_set_nonblock(fd);
    if (s->do_nodelay)
        socket_set_nodelay(fd);
    s->fd = fd;
    qemu_set_fd_handler2(s->listen_fd, NULL, NULL, NULL, NULL);
    tcp_chr_connect(chr);

    return 0;
}

static void tcp_chr_accept(void *opaque)
{
    CharDriverState *chr = opaque;
    TCPCharDriver *s = chr->opaque;
    struct sockaddr_in saddr;
#ifndef _WIN32
    struct sockaddr_un uaddr;
#endif
    struct sockaddr *addr;
    socklen_t len;
    int fd;

    for(;;) {
#ifndef _WIN32
	if (s->is_unix) {
	    len = sizeof(uaddr);
	    addr = (struct sockaddr *)&uaddr;
	} else
#endif
	{
	    len = sizeof(saddr);
	    addr = (struct sockaddr *)&saddr;
	}
        fd = qemu_accept(s->listen_fd, addr, &len);
        if (fd < 0 && errno != EINTR) {
            return;
        } else if (fd >= 0) {
            if (s->do_telnetopt)
                tcp_chr_telnet_init(fd);
            break;
        }
    }
    if (tcp_chr_add_client(chr, fd) < 0)
	close(fd);
}

static void tcp_chr_close(CharDriverState *chr)
{
    TCPCharDriver *s = chr->opaque;
    if (s->fd >= 0) {
        qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);
        closesocket(s->fd);
    }
    if (s->listen_fd >= 0) {
        qemu_set_fd_handler2(s->listen_fd, NULL, NULL, NULL, NULL);
        closesocket(s->listen_fd);
    }
    g_free(s);
    qemu_chr_be_event(chr, CHR_EVENT_CLOSED);
}

static CharDriverState *qemu_chr_open_socket_fd(int fd, bool do_nodelay,
                                                bool is_listen, bool is_telnet,
                                                bool is_waitconnect,
                                                Error **errp)
{
    CharDriverState *chr = NULL;
    TCPCharDriver *s = NULL;
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    const char *left = "", *right = "";
    struct sockaddr_storage ss;
    socklen_t ss_len = sizeof(ss);

    memset(&ss, 0, ss_len);
    if (getsockname(fd, (struct sockaddr *) &ss, &ss_len) != 0) {
        error_setg(errp, "getsockname: %s", strerror(errno));
        return NULL;
    }

    chr = g_malloc0(sizeof(CharDriverState));
    s = g_malloc0(sizeof(TCPCharDriver));

    s->connected = 0;
    s->fd = -1;
    s->listen_fd = -1;
    s->msgfd = -1;

    chr->filename = g_malloc(256);
    switch (ss.ss_family) {
#ifndef _WIN32
    case AF_UNIX:
        s->is_unix = 1;
        snprintf(chr->filename, 256, "unix:%s%s",
                 ((struct sockaddr_un *)(&ss))->sun_path,
                 is_listen ? ",server" : "");
        break;
#endif
    case AF_INET6:
        left  = "[";
        right = "]";
        /* fall through */
    case AF_INET:
        s->do_nodelay = do_nodelay;
        getnameinfo((struct sockaddr *) &ss, ss_len, host, sizeof(host),
                    serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);
        snprintf(chr->filename, 256, "%s:%s:%s%s%s%s",
                 is_telnet ? "telnet" : "tcp",
                 left, host, right, serv,
                 is_listen ? ",server" : "");
        break;
    }

    chr->opaque = s;
    chr->chr_write = tcp_chr_write;
    chr->chr_close = tcp_chr_close;
    chr->get_msgfd = tcp_get_msgfd;
    chr->chr_add_client = tcp_chr_add_client;

    if (is_listen) {
        s->listen_fd = fd;
        qemu_set_fd_handler2(s->listen_fd, NULL, tcp_chr_accept, NULL, chr);
        if (is_telnet) {
            s->do_telnetopt = 1;
        }
    } else {
        s->connected = 1;
        s->fd = fd;
        socket_set_nodelay(fd);
        tcp_chr_connect(chr);
    }

    if (is_listen && is_waitconnect) {
        printf("QEMU waiting for connection on: %s\n",
               chr->filename);
        tcp_chr_accept(chr);
        socket_set_nonblock(s->listen_fd);
    }
    return chr;
}

static CharDriverState *qemu_chr_open_socket(QemuOpts *opts)
{
    CharDriverState *chr = NULL;
    Error *local_err = NULL;
    int fd = -1;
    int is_listen;
    int is_waitconnect;
    int do_nodelay;
    int is_unix;
    int is_telnet;

    is_listen      = qemu_opt_get_bool(opts, "server", 0);
    is_waitconnect = qemu_opt_get_bool(opts, "wait", 1);
    is_telnet      = qemu_opt_get_bool(opts, "telnet", 0);
    do_nodelay     = !qemu_opt_get_bool(opts, "delay", 1);
    is_unix        = qemu_opt_get(opts, "path") != NULL;
    if (!is_listen)
        is_waitconnect = 0;

    if (is_unix) {
        if (is_listen) {
            fd = unix_listen_opts(opts, &local_err);
        } else {
            fd = unix_connect_opts(opts, &local_err, NULL, NULL);
        }
    } else {
        if (is_listen) {
            fd = inet_listen_opts(opts, 0, &local_err);
        } else {
            fd = inet_connect_opts(opts, &local_err, NULL, NULL);
        }
    }
    if (fd < 0) {
        goto fail;
    }

    if (!is_waitconnect)
        socket_set_nonblock(fd);

    chr = qemu_chr_open_socket_fd(fd, do_nodelay, is_listen, is_telnet,
                                  is_waitconnect, &local_err);
    if (error_is_set(&local_err)) {
        goto fail;
    }
    return chr;


 fail:
    if (local_err) {
        qerror_report_err(local_err);
        error_free(local_err);
    }
    if (fd >= 0) {
        closesocket(fd);
    }
    if (chr) {
        g_free(chr->opaque);
        g_free(chr);
    }
    return NULL;
}

/***********************************************************/
/* Memory chardev */
typedef struct {
    size_t outbuf_size;
    size_t outbuf_capacity;
    uint8_t *outbuf;
} MemoryDriver;

static int mem_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    MemoryDriver *d = chr->opaque;

    /* TODO: the QString implementation has the same code, we should
     * introduce a generic way to do this in cutils.c */
    if (d->outbuf_capacity < d->outbuf_size + len) {
        /* grow outbuf */
        d->outbuf_capacity += len;
        d->outbuf_capacity *= 2;
        d->outbuf = g_realloc(d->outbuf, d->outbuf_capacity);
    }

    memcpy(d->outbuf + d->outbuf_size, buf, len);
    d->outbuf_size += len;

    return len;
}

void qemu_chr_init_mem(CharDriverState *chr)
{
    MemoryDriver *d;

    d = g_malloc(sizeof(*d));
    d->outbuf_size = 0;
    d->outbuf_capacity = 4096;
    d->outbuf = g_malloc0(d->outbuf_capacity);

    memset(chr, 0, sizeof(*chr));
    chr->opaque = d;
    chr->chr_write = mem_chr_write;
}

QString *qemu_chr_mem_to_qs(CharDriverState *chr)
{
    MemoryDriver *d = chr->opaque;
    return qstring_from_substr((char *) d->outbuf, 0, d->outbuf_size - 1);
}

/* NOTE: this driver can not be closed with qemu_chr_delete()! */
void qemu_chr_close_mem(CharDriverState *chr)
{
    MemoryDriver *d = chr->opaque;

    g_free(d->outbuf);
    g_free(chr->opaque);
    chr->opaque = NULL;
    chr->chr_write = NULL;
}

size_t qemu_chr_mem_osize(const CharDriverState *chr)
{
    const MemoryDriver *d = chr->opaque;
    return d->outbuf_size;
}

/*********************************************************/
/* Ring buffer chardev */

typedef struct {
    size_t size;
    size_t prod;
    size_t cons;
    uint8_t *cbuf;
} RingBufCharDriver;

static size_t ringbuf_count(const CharDriverState *chr)
{
    const RingBufCharDriver *d = chr->opaque;

    return d->prod - d->cons;
}

static int ringbuf_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    RingBufCharDriver *d = chr->opaque;
    int i;

    if (!buf || (len < 0)) {
        return -1;
    }

    for (i = 0; i < len; i++ ) {
        d->cbuf[d->prod++ & (d->size - 1)] = buf[i];
        if (d->prod - d->cons > d->size) {
            d->cons = d->prod - d->size;
        }
    }

    return 0;
}

static int ringbuf_chr_read(CharDriverState *chr, uint8_t *buf, int len)
{
    RingBufCharDriver *d = chr->opaque;
    int i;

    for (i = 0; i < len && d->cons != d->prod; i++) {
        buf[i] = d->cbuf[d->cons++ & (d->size - 1)];
    }

    return i;
}

static void ringbuf_chr_close(struct CharDriverState *chr)
{
    RingBufCharDriver *d = chr->opaque;

    g_free(d->cbuf);
    g_free(d);
    chr->opaque = NULL;
}

static CharDriverState *qemu_chr_open_ringbuf(QemuOpts *opts)
{
    CharDriverState *chr;
    RingBufCharDriver *d;

    chr = g_malloc0(sizeof(CharDriverState));
    d = g_malloc(sizeof(*d));

    d->size = qemu_opt_get_size(opts, "size", 0);
    if (d->size == 0) {
        d->size = 65536;
    }

    /* The size must be power of 2 */
    if (d->size & (d->size - 1)) {
        error_report("size of ringbuf device must be power of two");
        goto fail;
    }

    d->prod = 0;
    d->cons = 0;
    d->cbuf = g_malloc0(d->size);

    chr->opaque = d;
    chr->chr_write = ringbuf_chr_write;
    chr->chr_close = ringbuf_chr_close;

    return chr;

fail:
    g_free(d);
    g_free(chr);
    return NULL;
}

static bool chr_is_ringbuf(const CharDriverState *chr)
{
    return chr->chr_write == ringbuf_chr_write;
}

void qmp_ringbuf_write(const char *device, const char *data,
                       bool has_format, enum DataFormat format,
                       Error **errp)
{
    CharDriverState *chr;
    const uint8_t *write_data;
    int ret;
    size_t write_count;

    chr = qemu_chr_find(device);
    if (!chr) {
        error_setg(errp, "Device '%s' not found", device);
        return;
    }

    if (!chr_is_ringbuf(chr)) {
        error_setg(errp,"%s is not a ringbuf device", device);
        return;
    }

    if (has_format && (format == DATA_FORMAT_BASE64)) {
        write_data = g_base64_decode(data, &write_count);
    } else {
        write_data = (uint8_t *)data;
        write_count = strlen(data);
    }

    ret = ringbuf_chr_write(chr, write_data, write_count);

    if (write_data != (uint8_t *)data) {
        g_free((void *)write_data);
    }

    if (ret < 0) {
        error_setg(errp, "Failed to write to device %s", device);
        return;
    }
}

char *qmp_ringbuf_read(const char *device, int64_t size,
                       bool has_format, enum DataFormat format,
                       Error **errp)
{
    CharDriverState *chr;
    uint8_t *read_data;
    size_t count;
    char *data;

    chr = qemu_chr_find(device);
    if (!chr) {
        error_setg(errp, "Device '%s' not found", device);
        return NULL;
    }

    if (!chr_is_ringbuf(chr)) {
        error_setg(errp,"%s is not a ringbuf device", device);
        return NULL;
    }

    if (size <= 0) {
        error_setg(errp, "size must be greater than zero");
        return NULL;
    }

    count = ringbuf_count(chr);
    size = size > count ? count : size;
    read_data = g_malloc(size + 1);

    ringbuf_chr_read(chr, read_data, size);

    if (has_format && (format == DATA_FORMAT_BASE64)) {
        data = g_base64_encode(read_data, size);
        g_free(read_data);
    } else {
        /*
         * FIXME should read only complete, valid UTF-8 characters up
         * to @size bytes.  Invalid sequences should be replaced by a
         * suitable replacement character.  Except when (and only
         * when) ring buffer lost characters since last read, initial
         * continuation characters should be dropped.
         */
        read_data[size] = 0;
        data = (char *)read_data;
    }

    return data;
}

QemuOpts *qemu_chr_parse_compat(const char *label, const char *filename)
{
    char host[65], port[33], width[8], height[8];
    int pos;
    const char *p;
    QemuOpts *opts;
    Error *local_err = NULL;

    opts = qemu_opts_create(qemu_find_opts("chardev"), label, 1, &local_err);
    if (error_is_set(&local_err)) {
        qerror_report_err(local_err);
        error_free(local_err);
        return NULL;
    }

    if (strstart(filename, "mon:", &p)) {
        filename = p;
        qemu_opt_set(opts, "mux", "on");
    }

    if (strcmp(filename, "null")    == 0 ||
        strcmp(filename, "pty")     == 0 ||
        strcmp(filename, "msmouse") == 0 ||
        strcmp(filename, "braille") == 0 ||
        strcmp(filename, "stdio")   == 0) {
        qemu_opt_set(opts, "backend", filename);
        return opts;
    }
    if (strstart(filename, "vc", &p)) {
        qemu_opt_set(opts, "backend", "vc");
        if (*p == ':') {
            if (sscanf(p+1, "%8[0-9]x%8[0-9]", width, height) == 2) {
                /* pixels */
                qemu_opt_set(opts, "width", width);
                qemu_opt_set(opts, "height", height);
            } else if (sscanf(p+1, "%8[0-9]Cx%8[0-9]C", width, height) == 2) {
                /* chars */
                qemu_opt_set(opts, "cols", width);
                qemu_opt_set(opts, "rows", height);
            } else {
                goto fail;
            }
        }
        return opts;
    }
    if (strcmp(filename, "con:") == 0) {
        qemu_opt_set(opts, "backend", "console");
        return opts;
    }
    if (strstart(filename, "COM", NULL)) {
        qemu_opt_set(opts, "backend", "serial");
        qemu_opt_set(opts, "path", filename);
        return opts;
    }
    if (strstart(filename, "file:", &p)) {
        qemu_opt_set(opts, "backend", "file");
        qemu_opt_set(opts, "path", p);
        return opts;
    }
    if (strstart(filename, "pipe:", &p)) {
        qemu_opt_set(opts, "backend", "pipe");
        qemu_opt_set(opts, "path", p);
        return opts;
    }
    if (strstart(filename, "tcp:", &p) ||
        strstart(filename, "telnet:", &p)) {
        if (sscanf(p, "%64[^:]:%32[^,]%n", host, port, &pos) < 2) {
            host[0] = 0;
            if (sscanf(p, ":%32[^,]%n", port, &pos) < 1)
                goto fail;
        }
        qemu_opt_set(opts, "backend", "socket");
        qemu_opt_set(opts, "host", host);
        qemu_opt_set(opts, "port", port);
        if (p[pos] == ',') {
            if (qemu_opts_do_parse(opts, p+pos+1, NULL) != 0)
                goto fail;
        }
        if (strstart(filename, "telnet:", &p))
            qemu_opt_set(opts, "telnet", "on");
        return opts;
    }
    if (strstart(filename, "udp:", &p)) {
        qemu_opt_set(opts, "backend", "udp");
        if (sscanf(p, "%64[^:]:%32[^@,]%n", host, port, &pos) < 2) {
            host[0] = 0;
            if (sscanf(p, ":%32[^@,]%n", port, &pos) < 1) {
                goto fail;
            }
        }
        qemu_opt_set(opts, "host", host);
        qemu_opt_set(opts, "port", port);
        if (p[pos] == '@') {
            p += pos + 1;
            if (sscanf(p, "%64[^:]:%32[^,]%n", host, port, &pos) < 2) {
                host[0] = 0;
                if (sscanf(p, ":%32[^,]%n", port, &pos) < 1) {
                    goto fail;
                }
            }
            qemu_opt_set(opts, "localaddr", host);
            qemu_opt_set(opts, "localport", port);
        }
        return opts;
    }
    if (strstart(filename, "unix:", &p)) {
        qemu_opt_set(opts, "backend", "socket");
        if (qemu_opts_do_parse(opts, p, "path") != 0)
            goto fail;
        return opts;
    }
    if (strstart(filename, "/dev/parport", NULL) ||
        strstart(filename, "/dev/ppi", NULL)) {
        qemu_opt_set(opts, "backend", "parport");
        qemu_opt_set(opts, "path", filename);
        return opts;
    }
    if (strstart(filename, "/dev/", NULL)) {
        qemu_opt_set(opts, "backend", "tty");
        qemu_opt_set(opts, "path", filename);
        return opts;
    }

fail:
    qemu_opts_del(opts);
    return NULL;
}

#ifdef HAVE_CHARDEV_PARPORT

static CharDriverState *qemu_chr_open_pp(QemuOpts *opts)
{
    const char *filename = qemu_opt_get(opts, "path");
    int fd;

    fd = qemu_open(filename, O_RDWR);
    if (fd < 0) {
        return NULL;
    }
    return qemu_chr_open_pp_fd(fd);
}

#endif

static const struct {
    const char *name;
    CharDriverState *(*open)(QemuOpts *opts);
} backend_table[] = {
    { .name = "null",      .open = qemu_chr_open_null },
    { .name = "socket",    .open = qemu_chr_open_socket },
    { .name = "udp",       .open = qemu_chr_open_udp },
    { .name = "msmouse",   .open = qemu_chr_open_msmouse },
    { .name = "vc",        .open = text_console_init },
    { .name = "memory",    .open = qemu_chr_open_ringbuf },
#ifdef _WIN32
    { .name = "file",      .open = qemu_chr_open_win_file_out },
    { .name = "pipe",      .open = qemu_chr_open_win_pipe },
    { .name = "console",   .open = qemu_chr_open_win_con },
    { .name = "serial",    .open = qemu_chr_open_win },
    { .name = "stdio",     .open = qemu_chr_open_win_stdio },
#else
    { .name = "file",      .open = qemu_chr_open_file_out },
    { .name = "pipe",      .open = qemu_chr_open_pipe },
    { .name = "stdio",     .open = qemu_chr_open_stdio },
#endif
#ifdef CONFIG_BRLAPI
    { .name = "braille",   .open = chr_baum_init },
#endif
#ifdef HAVE_CHARDEV_TTY
    { .name = "tty",       .open = qemu_chr_open_tty },
    { .name = "serial",    .open = qemu_chr_open_tty },
    { .name = "pty",       .open = qemu_chr_open_pty },
#endif
#ifdef HAVE_CHARDEV_PARPORT
    { .name = "parallel",  .open = qemu_chr_open_pp },
    { .name = "parport",   .open = qemu_chr_open_pp },
#endif
#ifdef CONFIG_SPICE
    { .name = "spicevmc",     .open = qemu_chr_open_spice },
#if SPICE_SERVER_VERSION >= 0x000c02
    { .name = "spiceport",    .open = qemu_chr_open_spice_port },
#endif
#endif
};

CharDriverState *qemu_chr_new_from_opts(QemuOpts *opts,
                                    void (*init)(struct CharDriverState *s),
                                    Error **errp)
{
    CharDriverState *chr;
    int i;

    if (qemu_opts_id(opts) == NULL) {
        error_setg(errp, "chardev: no id specified");
        goto err;
    }

    if (qemu_opt_get(opts, "backend") == NULL) {
        error_setg(errp, "chardev: \"%s\" missing backend",
                   qemu_opts_id(opts));
        goto err;
    }
    for (i = 0; i < ARRAY_SIZE(backend_table); i++) {
        if (strcmp(backend_table[i].name, qemu_opt_get(opts, "backend")) == 0)
            break;
    }
    if (i == ARRAY_SIZE(backend_table)) {
        error_setg(errp, "chardev: backend \"%s\" not found",
                   qemu_opt_get(opts, "backend"));
        goto err;
    }

    chr = backend_table[i].open(opts);
    if (!chr) {
        error_setg(errp, "chardev: opening backend \"%s\" failed",
                   qemu_opt_get(opts, "backend"));
        goto err;
    }

    if (!chr->filename)
        chr->filename = g_strdup(qemu_opt_get(opts, "backend"));
    chr->init = init;
    QTAILQ_INSERT_TAIL(&chardevs, chr, next);

    if (qemu_opt_get_bool(opts, "mux", 0)) {
        CharDriverState *base = chr;
        int len = strlen(qemu_opts_id(opts)) + 6;
        base->label = g_malloc(len);
        snprintf(base->label, len, "%s-base", qemu_opts_id(opts));
        chr = qemu_chr_open_mux(base);
        chr->filename = base->filename;
        chr->avail_connections = MAX_MUX;
        QTAILQ_INSERT_TAIL(&chardevs, chr, next);
    } else {
        chr->avail_connections = 1;
    }
    chr->label = g_strdup(qemu_opts_id(opts));
    chr->opts = opts;
    return chr;

err:
    qemu_opts_del(opts);
    return NULL;
}

CharDriverState *qemu_chr_new(const char *label, const char *filename, void (*init)(struct CharDriverState *s))
{
    const char *p;
    CharDriverState *chr;
    QemuOpts *opts;
    Error *err = NULL;

    if (strstart(filename, "chardev:", &p)) {
        return qemu_chr_find(p);
    }

    opts = qemu_chr_parse_compat(label, filename);
    if (!opts)
        return NULL;

    chr = qemu_chr_new_from_opts(opts, init, &err);
    if (error_is_set(&err)) {
        fprintf(stderr, "%s\n", error_get_pretty(err));
        error_free(err);
    }
    if (chr && qemu_opt_get_bool(opts, "mux", 0)) {
        monitor_init(chr, MONITOR_USE_READLINE);
    }
    return chr;
}

void qemu_chr_fe_set_echo(struct CharDriverState *chr, bool echo)
{
    if (chr->chr_set_echo) {
        chr->chr_set_echo(chr, echo);
    }
}

void qemu_chr_fe_open(struct CharDriverState *chr)
{
    if (chr->chr_guest_open) {
        chr->chr_guest_open(chr);
    }
}

void qemu_chr_fe_close(struct CharDriverState *chr)
{
    if (chr->chr_guest_close) {
        chr->chr_guest_close(chr);
    }
}

void qemu_chr_delete(CharDriverState *chr)
{
    QTAILQ_REMOVE(&chardevs, chr, next);
    if (chr->chr_close) {
        chr->chr_close(chr);
    }
    g_free(chr->filename);
    g_free(chr->label);
    if (chr->opts) {
        qemu_opts_del(chr->opts);
    }
    g_free(chr);
}

ChardevInfoList *qmp_query_chardev(Error **errp)
{
    ChardevInfoList *chr_list = NULL;
    CharDriverState *chr;

    QTAILQ_FOREACH(chr, &chardevs, next) {
        ChardevInfoList *info = g_malloc0(sizeof(*info));
        info->value = g_malloc0(sizeof(*info->value));
        info->value->label = g_strdup(chr->label);
        info->value->filename = g_strdup(chr->filename);

        info->next = chr_list;
        chr_list = info;
    }

    return chr_list;
}

CharDriverState *qemu_chr_find(const char *name)
{
    CharDriverState *chr;

    QTAILQ_FOREACH(chr, &chardevs, next) {
        if (strcmp(chr->label, name) != 0)
            continue;
        return chr;
    }
    return NULL;
}

/* Get a character (serial) device interface.  */
CharDriverState *qemu_char_get_next_serial(void)
{
    static int next_serial;

    /* FIXME: This function needs to go away: use chardev properties!  */
    return serial_hds[next_serial++];
}

QemuOptsList qemu_chardev_opts = {
    .name = "chardev",
    .implied_opt_name = "backend",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_chardev_opts.head),
    .desc = {
        {
            .name = "backend",
            .type = QEMU_OPT_STRING,
        },{
            .name = "path",
            .type = QEMU_OPT_STRING,
        },{
            .name = "host",
            .type = QEMU_OPT_STRING,
        },{
            .name = "port",
            .type = QEMU_OPT_STRING,
        },{
            .name = "localaddr",
            .type = QEMU_OPT_STRING,
        },{
            .name = "localport",
            .type = QEMU_OPT_STRING,
        },{
            .name = "to",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "ipv4",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "ipv6",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "wait",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "server",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "delay",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "telnet",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "width",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "height",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "cols",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "rows",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "mux",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "signal",
            .type = QEMU_OPT_BOOL,
        },{
            .name = "name",
            .type = QEMU_OPT_STRING,
        },{
            .name = "debug",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "size",
            .type = QEMU_OPT_SIZE,
        },
        { /* end of list */ }
    },
};

#ifdef _WIN32

static CharDriverState *qmp_chardev_open_file(ChardevFile *file, Error **errp)
{
    HANDLE out;

    if (file->in) {
        error_setg(errp, "input file not supported");
        return NULL;
    }

    out = CreateFile(file->out, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                     OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        error_setg(errp, "open %s failed", file->out);
        return NULL;
    }
    return qemu_chr_open_win_file(out);
}

static CharDriverState *qmp_chardev_open_serial(ChardevHostdev *serial,
                                                Error **errp)
{
    return qemu_chr_open_win_path(serial->device);
}

static CharDriverState *qmp_chardev_open_parallel(ChardevHostdev *parallel,
                                                  Error **errp)
{
    error_setg(errp, "character device backend type 'parallel' not supported");
    return NULL;
}

#else /* WIN32 */

static int qmp_chardev_open_file_source(char *src, int flags,
                                        Error **errp)
{
    int fd = -1;

    TFR(fd = qemu_open(src, flags, 0666));
    if (fd == -1) {
        error_setg(errp, "open %s: %s", src, strerror(errno));
    }
    return fd;
}

static CharDriverState *qmp_chardev_open_file(ChardevFile *file, Error **errp)
{
    int flags, in = -1, out = -1;

    flags = O_WRONLY | O_TRUNC | O_CREAT | O_BINARY;
    out = qmp_chardev_open_file_source(file->out, flags, errp);
    if (error_is_set(errp)) {
        return NULL;
    }

    if (file->in) {
        flags = O_RDONLY;
        in = qmp_chardev_open_file_source(file->in, flags, errp);
        if (error_is_set(errp)) {
            qemu_close(out);
            return NULL;
        }
    }

    return qemu_chr_open_fd(in, out);
}

static CharDriverState *qmp_chardev_open_serial(ChardevHostdev *serial,
                                                Error **errp)
{
#ifdef HAVE_CHARDEV_TTY
    int fd;

    fd = qmp_chardev_open_file_source(serial->device, O_RDWR, errp);
    if (error_is_set(errp)) {
        return NULL;
    }
    socket_set_nonblock(fd);
    return qemu_chr_open_tty_fd(fd);
#else
    error_setg(errp, "character device backend type 'serial' not supported");
    return NULL;
#endif
}

static CharDriverState *qmp_chardev_open_parallel(ChardevHostdev *parallel,
                                                  Error **errp)
{
#ifdef HAVE_CHARDEV_PARPORT
    int fd;

    fd = qmp_chardev_open_file_source(parallel->device, O_RDWR, errp);
    if (error_is_set(errp)) {
        return NULL;
    }
    return qemu_chr_open_pp_fd(fd);
#else
    error_setg(errp, "character device backend type 'parallel' not supported");
    return NULL;
#endif
}

#endif /* WIN32 */

static CharDriverState *qmp_chardev_open_socket(ChardevSocket *sock,
                                                Error **errp)
{
    SocketAddress *addr = sock->addr;
    bool do_nodelay     = sock->has_nodelay ? sock->nodelay : false;
    bool is_listen      = sock->has_server  ? sock->server  : true;
    bool is_telnet      = sock->has_telnet  ? sock->telnet  : false;
    bool is_waitconnect = sock->has_wait    ? sock->wait    : false;
    int fd;

    if (is_listen) {
        fd = socket_listen(addr, errp);
    } else {
        fd = socket_connect(addr, errp, NULL, NULL);
    }
    if (error_is_set(errp)) {
        return NULL;
    }
    return qemu_chr_open_socket_fd(fd, do_nodelay, is_listen,
                                   is_telnet, is_waitconnect, errp);
}

ChardevReturn *qmp_chardev_add(const char *id, ChardevBackend *backend,
                               Error **errp)
{
    ChardevReturn *ret = g_new0(ChardevReturn, 1);
    CharDriverState *chr = NULL;

    chr = qemu_chr_find(id);
    if (chr) {
        error_setg(errp, "Chardev '%s' already exists", id);
        g_free(ret);
        return NULL;
    }

    switch (backend->kind) {
    case CHARDEV_BACKEND_KIND_FILE:
        chr = qmp_chardev_open_file(backend->file, errp);
        break;
    case CHARDEV_BACKEND_KIND_SERIAL:
        chr = qmp_chardev_open_serial(backend->serial, errp);
        break;
    case CHARDEV_BACKEND_KIND_PARALLEL:
        chr = qmp_chardev_open_parallel(backend->parallel, errp);
        break;
    case CHARDEV_BACKEND_KIND_SOCKET:
        chr = qmp_chardev_open_socket(backend->socket, errp);
        break;
#ifdef HAVE_CHARDEV_TTY
    case CHARDEV_BACKEND_KIND_PTY:
    {
        /* qemu_chr_open_pty sets "path" in opts */
        QemuOpts *opts;
        opts = qemu_opts_create_nofail(qemu_find_opts("chardev"));
        chr = qemu_chr_open_pty(opts);
        ret->pty = g_strdup(qemu_opt_get(opts, "path"));
        ret->has_pty = true;
        qemu_opts_del(opts);
        break;
    }
#endif
    case CHARDEV_BACKEND_KIND_NULL:
        chr = qemu_chr_open_null(NULL);
        break;
    default:
        error_setg(errp, "unknown chardev backend (%d)", backend->kind);
        break;
    }

    if (chr == NULL && !error_is_set(errp)) {
        error_setg(errp, "Failed to create chardev");
    }
    if (chr) {
        chr->label = g_strdup(id);
        chr->avail_connections = 1;
        QTAILQ_INSERT_TAIL(&chardevs, chr, next);
        return ret;
    } else {
        g_free(ret);
        return NULL;
    }
}

void qmp_chardev_remove(const char *id, Error **errp)
{
    CharDriverState *chr;

    chr = qemu_chr_find(id);
    if (NULL == chr) {
        error_setg(errp, "Chardev '%s' not found", id);
        return;
    }
    if (chr->chr_can_read || chr->chr_read ||
        chr->chr_event || chr->handler_opaque) {
        error_setg(errp, "Chardev '%s' is busy", id);
        return;
    }
    qemu_chr_delete(chr);
}
