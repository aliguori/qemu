/*
 * QEMU Random Number Generator Backend
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/rng.h"
#include "qerror.h"

#define TYPE_RNG_URANDOM "rng-urandom"
#define RNG_URANDOM(obj) OBJECT_CHECK(RndURandom, (obj), TYPE_RNG_URANDOM)

typedef struct RndURandom
{
    RngBackend parent;

    int fd;
    char *filename;
    GSList *pending_data;
} RndURandom;

/**
 * A simple and incomplete backend to request entropy from /dev/urandom.
 *
 * This backend exposes an additional "filename" property that can be used to
 * set the filename to use to open the backend.
 */
static void rng_urandom_request_entropy(RngBackend *b, size_t size,
                                        EntropyReceiveFunc *receive_entropy,
                                        void *opaque)
{
    RndURandom *s = RNG_URANDOM(b);
    uint8_t data[size];
    ssize_t ret;

    g_assert(s->fd != -1);

    do {
        ret = read(s->fd, data, size);
    } while (ret == -1 && errno == EINTR);

    g_assert(ret != -1);

    /* FIXME: needs to be done via a BH */
    receive_entropy(opaque, data, ret);
}

static void rng_urandom_opened(RngBackend *b, Error **errp)
{
    RndURandom *s = RNG_URANDOM(b);

    if (s->filename == NULL) {
        error_set(errp, QERR_INVALID_PARAMETER_VALUE,
                  "filename", "a valid filename");
    } else {
        s->fd = open(s->filename, O_RDONLY);

        if (s->fd == -1) {
            error_set(errp, QERR_OPEN_FILE_FAILED, s->filename);
        }
    }
}

static char *rng_urandom_get_filename(Object *obj, Error **errp)
{
    RndURandom *s = RNG_URANDOM(obj);

    if (s->filename) {
        return g_strdup(s->filename);
    }

    return NULL;
}

static void rng_urandom_set_filename(Object *obj, const char *filename,
                                 Error **errp)
{
    RngBackend *b = RNG_BACKEND(obj);
    RndURandom *s = RNG_URANDOM(obj);

    if (b->opened) {
        error_set(errp, QERR_PERMISSION_DENIED);
        return;
    }

    if (s->filename) {
        g_free(s->filename);
    }

    s->filename = g_strdup(filename);
}

static void rng_urandom_init(Object *obj)
{
    RndURandom *s = RNG_URANDOM(obj);

    object_property_add_str(obj, "filename",
                            rng_urandom_get_filename,
                            rng_urandom_set_filename,
                            NULL);

    s->filename = g_strdup("/dev/urandom");
}

static void rng_urandom_finalize(Object *obj)
{
    RndURandom *s = RNG_URANDOM(obj);

    if (s->fd != -1) {
        close(s->fd);
    }

    g_free(s->filename);
}

static void rng_urandom_class_init(ObjectClass *klass, void *data)
{
    RngBackendClass *rbc = RNG_BACKEND_CLASS(klass);

    rbc->request_entropy = rng_urandom_request_entropy;
    rbc->opened = rng_urandom_opened;
}

static TypeInfo rng_urandom_info = {
    .name = TYPE_RNG_URANDOM,
    .parent = TYPE_RNG_BACKEND,
    .instance_size = sizeof(RndURandom),
    .class_init = rng_urandom_class_init,
    .instance_init = rng_urandom_init,
    .instance_finalize = rng_urandom_finalize,
};

static void register_types(void)
{
    type_register_static(&rng_urandom_info);
}

type_init(register_types);
