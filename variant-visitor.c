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

#include "qemu/variant-visitor.h"

static void vv_start_struct(Visitor *v, void **obj, const char *kind,
                            const char *name, size_t size, Error **errp)
{
}

static void vv_end_struct(Visitor *v, Error **errp)
{
}

static void vv_start_list(Visitor *v, const char *name, Error **errp)
{
}

static GenericList *vv_next_list(Visitor *v, GenericList **list, Error **errp)
{
    return NULL;
}

static void vv_end_list(Visitor *v, Error **errp)
{
}

static void vv_type_enum(Visitor *v, int *obj, const char *strings[],
                         const char *kind, const char *name, Error **errp)
{
    VariantVisitor *vv = container_of(v, VariantVisitor, parent);

    if (vv->kind == VV_NONE) {
        variant_visitor_set_enum(vv, *obj);
    } else {
        *obj = variant_visitor_get_enum(vv);
    }
}

static void vv_type_int(Visitor *v, int64_t *obj, const char *name, Error **errp)
{
    VariantVisitor *vv = container_of(v, VariantVisitor, parent);

    if (vv->kind == VV_NONE) {
        variant_visitor_set_int(vv, *obj);
    } else {
        *obj = variant_visitor_get_int(vv);
    }
}

static void vv_type_bool(Visitor *v, bool *obj, const char *name, Error **errp)
{
    VariantVisitor *vv = container_of(v, VariantVisitor, parent);

    if (vv->kind == VV_NONE) {
        variant_visitor_set_bool(vv, *obj);
    } else {
        *obj = variant_visitor_get_bool(vv);
    }
}

static void vv_type_str(Visitor *v, char **obj, const char *name, Error **errp)
{
    VariantVisitor *vv = container_of(v, VariantVisitor, parent);

    if (vv->kind == VV_NONE) {
        variant_visitor_set_string(vv, *obj);
    } else {
        *obj = g_strdup(variant_visitor_get_string(vv));
    }
}

static void vv_type_number(Visitor *v, double *obj, const char *name,
                           Error **errp)
{
    VariantVisitor *vv = container_of(v, VariantVisitor, parent);

    if (vv->kind == VV_NONE) {
        variant_visitor_set_number(vv, *obj);
    } else {
        *obj = variant_visitor_get_number(vv);
    }
}

void variant_visitor_init(VariantVisitor *v)
{
    v->parent.start_struct = vv_start_struct;
    v->parent.end_struct = vv_end_struct;

    v->parent.start_list = vv_start_list;
    v->parent.next_list = vv_next_list;
    v->parent.end_list = vv_end_list;

    v->parent.type_enum = vv_type_enum;
    v->parent.type_int = vv_type_int;
    v->parent.type_bool = vv_type_bool;
    v->parent.type_str = vv_type_str;
    v->parent.type_number = vv_type_number;

    v->kind = VV_NONE;
}

void variant_visitor_finalize(VariantVisitor *v)
{
    if (v->kind == VV_STRING) {
        g_free(v->v_string);
    }
}

void variant_visitor_set_string(VariantVisitor *v, const char *value)
{
    g_assert(v->kind == VV_NONE);
    v->kind = VV_STRING;
    v->v_string = g_strdup(value);
}

void variant_visitor_set_int(VariantVisitor *v, int64_t value)
{
    g_assert(v->kind == VV_NONE);
    v->kind = VV_INT;
    v->v_int = value;
}

void variant_visitor_set_bool(VariantVisitor *v, bool value)
{
    g_assert(v->kind == VV_NONE);
    v->kind = VV_BOOL;
    v->v_bool = value;
}

void variant_visitor_set_number(VariantVisitor *v, double value)
{
    g_assert(v->kind == VV_NONE);
    v->kind = VV_NUMBER;
    v->v_number = value;
}

void variant_visitor_set_enum(VariantVisitor *v, int value)
{
    g_assert(v->kind == VV_NONE);
    v->kind = VV_ENUM;
    v->v_enum = value;
}

const char *variant_visitor_get_string(VariantVisitor *v)
{
    g_assert(v->kind == VV_STRING);
    return v->v_string;
}

int64_t variant_visitor_get_int(VariantVisitor *v)
{
    g_assert(v->kind == VV_INT);
    return v->v_int;
}

bool variant_visitor_get_bool(VariantVisitor *v)
{
    g_assert(v->kind == VV_BOOL);
    return v->v_bool;
}

double variant_visitor_get_number(VariantVisitor *v)
{
    g_assert(v->kind == VV_NUMBER);
    return v->v_number;
}

int variant_visitor_get_enum(VariantVisitor *v)
{
    g_assert(v->kind == VV_ENUM);
    return v->v_enum;
}
