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

static void vty_ping(void)
{
    const char *greeting = "Hello, world!\n";
    int i;

    for (i = 0; greeting[i]; i++) {
        spapr_hcall3(H_PUT_TERM_CHAR, 0, 1, (uint64_t)greeting[i] << 56);
    }
}

int main(int argc, char **argv)
{
    QTestState *s = NULL;
    int ret;

    g_test_init(&argc, &argv, NULL);

    s = qtest_start("-display none -serial stdio -machine pseries");

    qtest_add_func("/vty/ping", vty_ping);
    ret = g_test_run();

    if (s) {
        qtest_quit(s);
    }

    return ret;
}
