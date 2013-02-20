/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (C) 2013  IBM, Corp.
 *
 * Authors:
 *   Anthony Liguori    <aliguori@us.ibm.com>
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
#include "qemu/main-loop.h"
#include "sysemu/sysemu.h"
#include "sysemu/qtest.h"
#include "sysemu/cpus.h"
#include "monitor/monitor.h"
#include "hw/hw.h"
#include "hw/boards.h"

/* reset/shutdown handler */

typedef struct QEMUResetEntry {
    QTAILQ_ENTRY(QEMUResetEntry) entry;
    QEMUResetHandler *func;
    void *opaque;
} QEMUResetEntry;

static QTAILQ_HEAD(reset_handlers, QEMUResetEntry) reset_handlers =
    QTAILQ_HEAD_INITIALIZER(reset_handlers);
static int shutdown_signal = -1;
static pid_t shutdown_pid;
static NotifierList powerdown_notifiers =
    NOTIFIER_LIST_INITIALIZER(powerdown_notifiers);
static NotifierList suspend_notifiers =
    NOTIFIER_LIST_INITIALIZER(suspend_notifiers);
static NotifierList wakeup_notifiers =
    NOTIFIER_LIST_INITIALIZER(wakeup_notifiers);
static uint32_t wakeup_reason_mask = ~0;

int qemu_shutdown_requested_get(void)
{
    return 0; // FIXME
}

int qemu_reset_requested_get(void)
{
    return 0; // FIXME
}

static void qemu_kill_report(void)
{
    if (!qtest_enabled() && shutdown_signal != -1) {
        fprintf(stderr, "qemu: terminating on signal %d", shutdown_signal);
        if (shutdown_pid == 0) {
            /* This happens for eg ^C at the terminal, so it's worth
             * avoiding printing an odd message in that case.
             */
            fputc('\n', stderr);
        } else {
            fprintf(stderr, " from pid " FMT_pid "\n", shutdown_pid);
        }
        shutdown_signal = -1;
    }
}

typedef struct SystemRequestState
{
    QuiescedFunc *request;
    void *opaque;
} SystemRequestState;

static gboolean qemu_quiesced_request(gpointer opaque)
{
    SystemRequestState *s = opaque;

    pause_all_vcpus();
    cpu_synchronize_all_states();
    if (s->request(s->opaque)) {
        resume_all_vcpus();
    }

    g_free(s);
    return FALSE;
}

guint qemu_idle_add_quiesced(QuiescedFunc *func, void *opaque)
{
    SystemRequestState *s = g_new0(SystemRequestState, 1);

    s->request = func;
    s->opaque = opaque;

    cpu_stop_current();
    return g_idle_add(qemu_quiesced_request, s);
}

static bool qemu_reset(void *unused)
{
    qemu_system_reset(VMRESET_REPORT);
    if (runstate_check(RUN_STATE_INTERNAL_ERROR) ||
        runstate_check(RUN_STATE_SHUTDOWN)) {
        runstate_set(RUN_STATE_PAUSED);
    }
    return true;
}

static bool qemu_wakeup(void *unused)
{
    qemu_system_reset(VMRESET_SILENT);
    monitor_protocol_event(QEVENT_WAKEUP, NULL);
    return true;
}

void qemu_register_reset(QEMUResetHandler *func, void *opaque)
{
    QEMUResetEntry *re = g_malloc0(sizeof(QEMUResetEntry));

    re->func = func;
    re->opaque = opaque;
    QTAILQ_INSERT_TAIL(&reset_handlers, re, entry);
}

void qemu_unregister_reset(QEMUResetHandler *func, void *opaque)
{
    QEMUResetEntry *re;

    QTAILQ_FOREACH(re, &reset_handlers, entry) {
        if (re->func == func && re->opaque == opaque) {
            QTAILQ_REMOVE(&reset_handlers, re, entry);
            g_free(re);
            return;
        }
    }
}

void qemu_devices_reset(void)
{
    QEMUResetEntry *re, *nre;

    cpu_reset_all();

    /* reset all devices */
    QTAILQ_FOREACH_SAFE(re, &reset_handlers, entry, nre) {
        re->func(re->opaque);
    }
}

void qemu_system_reset(bool report)
{
    if (current_machine && current_machine->reset) {
        current_machine->reset();
    } else {
        qemu_devices_reset();
    }
    if (report) {
        monitor_protocol_event(QEVENT_RESET, NULL);
    }
    cpu_synchronize_all_post_reset();
}

void qemu_system_reset_request(void)
{
    if (no_reboot) {
        qemu_system_shutdown_request();
    } else {
        qemu_idle_add_quiesced(qemu_reset, NULL);
    }
}

static bool qemu_system_suspend(void *unused)
{
    notifier_list_notify(&suspend_notifiers, NULL);
    runstate_set(RUN_STATE_SUSPENDED);
    monitor_protocol_event(QEVENT_SUSPEND, NULL);
    return false;
}

void qemu_system_suspend_request(void)
{
    if (runstate_check(RUN_STATE_SUSPENDED)) {
        return;
    }
    qemu_idle_add_quiesced(qemu_system_suspend, NULL);
}

void qemu_register_suspend_notifier(Notifier *notifier)
{
    notifier_list_add(&suspend_notifiers, notifier);
}

void qemu_system_wakeup_request(WakeupReason reason)
{
    if (!runstate_check(RUN_STATE_SUSPENDED)) {
        return;
    }
    if (!(wakeup_reason_mask & (1 << reason))) {
        return;
    }
    runstate_set(RUN_STATE_RUNNING);
    notifier_list_notify(&wakeup_notifiers, &reason);
    qemu_idle_add_quiesced(qemu_wakeup, NULL);
}

void qemu_system_wakeup_enable(WakeupReason reason, bool enabled)
{
    if (enabled) {
        wakeup_reason_mask |= (1 << reason);
    } else {
        wakeup_reason_mask &= ~(1 << reason);
    }
}

void qemu_register_wakeup_notifier(Notifier *notifier)
{
    notifier_list_add(&wakeup_notifiers, notifier);
}

void qemu_system_killed(int signal, pid_t pid)
{
    shutdown_signal = signal;
    shutdown_pid = pid;
    no_shutdown = 0;
    qemu_system_shutdown_request();
}

static bool qemu_system_shutdown(void *unused)
{
    qemu_kill_report();
    monitor_protocol_event(QEVENT_SHUTDOWN, NULL);
    if (no_shutdown) {
        vm_stop(RUN_STATE_SHUTDOWN);
    } else {
        main_loop_quit();
    }
    return false;
}

void qemu_system_shutdown_request(void)
{
    qemu_idle_add_quiesced(qemu_system_shutdown, NULL);
}

static bool qemu_system_powerdown(void *unused)
{
    monitor_protocol_event(QEVENT_POWERDOWN, NULL);
    notifier_list_notify(&powerdown_notifiers, NULL);
    return false;
}

void qemu_system_powerdown_request(void)
{
    qemu_idle_add_quiesced(qemu_system_powerdown, NULL);
}

void qemu_register_powerdown_notifier(Notifier *notifier)
{
    notifier_list_add(&powerdown_notifiers, notifier);
}

static bool qemu_system_debug(void *unused)
{
    vm_stop(RUN_STATE_DEBUG);
    return false;
}

void qemu_system_debug_request(void)
{
    qemu_idle_add_quiesced(qemu_system_debug, NULL);
}

static bool qemu_system_vmstop(void *opaque)
{
    RunState r = (RunState)opaque;
    vm_stop(r);
    return false;
}

void qemu_system_vmstop_request(RunState state)
{
    qemu_idle_add_quiesced(qemu_system_vmstop, (void *)state);
}
