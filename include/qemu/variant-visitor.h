/*
 * QEMU Object Model
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

#ifndef QEMU_VARIANT_VISITOR_H
#define QEMU_VARIANT_VISITOR_H

#include "qapi/qapi-visit-core.h"

typedef struct VariantVisitor
{
    Visitor parent;

    enum {
        VV_NONE,
        VV_INT,
        VV_STRING,
        VV_BOOL,
        VV_NUMBER,
        VV_ENUM,
    } kind;

    union {
        int64_t v_int;
        char *v_string;
        bool v_bool;
        double v_number;
        int v_enum;
    };
} VariantVisitor;

void variant_visitor_init(VariantVisitor *v);
void variant_visitor_finalize(VariantVisitor *v);

void variant_visitor_set_string(VariantVisitor *v, const char *value);
void variant_visitor_set_int(VariantVisitor *v, int64_t value);
void variant_visitor_set_bool(VariantVisitor *v, bool value);
void variant_visitor_set_number(VariantVisitor *v, double value);
void variant_visitor_set_enum(VariantVisitor *v, int value);

const char *variant_visitor_get_string(VariantVisitor *v);
int64_t variant_visitor_get_int(VariantVisitor *v);
bool variant_visitor_get_bool(VariantVisitor *v);
double variant_visitor_get_number(VariantVisitor *v);
int variant_visitor_get_enum(VariantVisitor *v);

#endif
