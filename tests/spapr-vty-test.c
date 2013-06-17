/*
 * QTest testcase for the sPAPR VTY
 *
 * Copyright IBM, Corp. 2013
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include "libqtest.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define H_GET_TERM_CHAR         0x54
#define H_PUT_TERM_CHAR         0x58

static char *qmp_ringbuf_read(const char *device, int max_len)
{
    size_t len;
    char *ret;
    char *ptr;

    ret = qtest_qmp(global_qtest,
                    "{ 'execute': 'ringbuf-read', "
                    "  'arguments': { 'device': '%s', 'size': %d } }",
                    device, max_len);
    len = strlen(ret);

    /* Skip pass {"return" */
    ptr = strchr(ret, '"');
    g_assert(ptr != NULL);
    ptr = strchr(ptr + 1, '"');
    g_assert(ptr != NULL);

    /* Start of data */
    ptr = strchr(ptr + 1, '"');
    g_assert(ptr != NULL);
    ptr += 1;

    len -= ptr - ret;
    memmove(ret, ptr, len);
    ret[len] = 0;

    ptr = strrchr(ret, '"');
    g_assert(ptr != NULL);
    *ptr = 0;

    return ret;
}

static void vty_ping(void)
{
    const char *greeting = "Hello, world!";
    char *data;
    int i;

    for (i = 0; greeting[i]; i++) {
        spapr_hcall3(H_PUT_TERM_CHAR, 0, 1, (uint64_t)greeting[i] << 56);
    }

    data = qmp_ringbuf_read("ring0", 16);
    g_assert_cmpstr(data, ==, greeting);
}

int main(int argc, char **argv)
{
    QTestState *s = NULL;
    int ret;

    g_test_init(&argc, &argv, NULL);

    s = qtest_start("-display none -serial chardev:ring0 "
                    "-machine pseries -chardev memory,id=ring0");

    qtest_add_func("/vty/ping", vty_ping);
    ret = g_test_run();

    if (s) {
        qtest_quit(s);
    }

    return ret;
}
