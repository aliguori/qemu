/*
 * QTest
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#ifndef LIBQTEST_H
#define LIBQTEST_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

typedef struct QTestState QTestState;

extern QTestState *global_qtest;

QTestState *qtest_init(const char *extra_args);
void qtest_quit(QTestState *s);

bool qtest_get_irq(QTestState *s, int num);

void qtest_irq_intercept_in(QTestState *s, const char *string);

void qtest_irq_intercept_out(QTestState *s, const char *string);

void qtest_outb(QTestState *s, uint16_t addr, uint8_t value);

void qtest_outw(QTestState *s, uint16_t addr, uint16_t value);

void qtest_outl(QTestState *s, uint16_t addr, uint32_t value);

uint8_t qtest_inb(QTestState *s, uint16_t addr);

uint16_t qtest_inw(QTestState *s, uint16_t addr);

uint32_t qtest_inl(QTestState *s, uint16_t addr);

void qtest_memread(QTestState *s, uint64_t addr, void *data, size_t size);

void qtest_memwrite(QTestState *s, uint64_t addr, const void *data, size_t size);

const char *qtest_get_arch(void);

void qtest_add_func(const char *str, void (*fn));

#define qtest_start(args) (            \
    global_qtest = qtest_init((args)) \
        )

#define get_irq(num) qtest_get_irq(global_qtest, (num))
#define irq_intercept_in(num) qtest_irq_intercept_in(global_qtest, (num))
#define irq_intercept_out(num) qtest_irq_intercept_out(global_qtest, (num))
#define outb(addr, val) qtest_outb(global_qtest, (addr), (val))
#define outw(addr, val) qtest_outw(global_qtest, (addr), (val))
#define outl(addr, val) qtest_outl(global_qtest, (addr), (val))
#define inb(addr) qtest_inb(global_qtest, (addr))
#define inw(addr) qtest_inw(global_qtest, (addr))
#define inl(addr) qtest_inl(global_qtest, (addr))
#define memread(addr, data, size) qtest_memread(global_qtest, (addr), (data), (size))
#define memwrite(addr, data, size) qtest_memwrite(global_qtest, (addr), (data), (size))

#endif
