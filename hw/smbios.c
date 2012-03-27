/*
 * SMBIOS Support
 *
 * Copyright (C) 2009 Hewlett-Packard Development Company, L.P.
 *
 * Authors:
 *  Alex Williamson <alex.williamson@hp.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "sysemu.h"
#include "arch_init.h"
#include "smbios.h"
#include "loader.h"

/*
 * Structures shared with the BIOS
 */
struct smbios_header {
    uint16_t length;
    uint8_t type;
} QEMU_PACKED;

struct smbios_field {
    struct smbios_header header;
    uint8_t type;
    uint16_t offset;
    uint8_t data[];
} QEMU_PACKED;

struct smbios_table {
    struct smbios_header header;
    uint8_t data[];
} QEMU_PACKED;

#define SMBIOS_FIELD_ENTRY 0
#define SMBIOS_TABLE_ENTRY 1


static uint8_t *smbios_entries;
static size_t smbios_entries_len;
static int smbios_type4_count = 0;

static void smbios_validate_table(void)
{
    if (smbios_type4_count && smbios_type4_count != smp_cpus) {
         fprintf(stderr,
                 "Number of SMBIOS Type 4 tables must match cpu count.\n");
        exit(1);
    }
}

uint8_t *smbios_get_table(size_t *length)
{
    smbios_validate_table();
    *length = smbios_entries_len;
    return smbios_entries;
}

/*
 * To avoid unresolvable overlaps in data, don't allow both
 * tables and fields for the same smbios type.
 */
static void smbios_check_collision(int type, int entry)
{
    uint16_t *num_entries = (uint16_t *)smbios_entries;
    struct smbios_header *header;
    char *p;
    int i;

    if (!num_entries)
        return;

    p = (char *)(num_entries + 1);

    for (i = 0; i < *num_entries; i++) {
        header = (struct smbios_header *)p;
        if (entry == SMBIOS_TABLE_ENTRY && header->type == SMBIOS_FIELD_ENTRY) {
            struct smbios_field *field = (void *)header;
            if (type == field->type) {
                fprintf(stderr, "SMBIOS type %d field already defined, "
                                "cannot add table\n", type);
                exit(1);
            }
        } else if (entry == SMBIOS_FIELD_ENTRY &&
                   header->type == SMBIOS_TABLE_ENTRY) {
            struct smbios_structure_header *table = (void *)(header + 1);
            if (type == table->type) {
                fprintf(stderr, "SMBIOS type %d table already defined, "
                                "cannot add field\n", type);
                exit(1);
            }
        }
        p += le16_to_cpu(header->length);
    }
}

void smbios_add_field(int type, int offset, int len, const void *data)
{
    struct smbios_field *field;

    smbios_check_collision(type, SMBIOS_FIELD_ENTRY);

    if (!smbios_entries) {
        smbios_entries_len = sizeof(uint16_t);
        smbios_entries = g_malloc0(smbios_entries_len);
    }
    smbios_entries = g_realloc(smbios_entries, smbios_entries_len +
                                                  sizeof(*field) + len);
    field = (struct smbios_field *)(smbios_entries + smbios_entries_len);
    field->header.type = SMBIOS_FIELD_ENTRY;
    field->header.length = cpu_to_le16(sizeof(*field) + len);

    field->type = type;
    field->offset = cpu_to_le16(offset);
    memcpy(field->data, data, len);

    smbios_entries_len += sizeof(*field) + len;
    (*(uint16_t *)smbios_entries) =
            cpu_to_le16(le16_to_cpu(*(uint16_t *)smbios_entries) + 1);
}

static QemuOptDesc smbios_type_0_desc[] = {
    {
        .name = "type",
        .type = QEMU_OPT_NUMBER,
    },
    {
        .name = "vendor",
        .type = QEMU_OPT_STRING,
    },
    {
        .name = "version",
        .type = QEMU_OPT_STRING,
    },
    {
        .name = "date",
        .type = QEMU_OPT_STRING,
    },
    {
        .name = "release",
        .type = QEMU_OPT_STRING,
    },
    { /* end of list */ }
};

static int smbios_build_type_0_fields(QemuOpts *opts)
{
    const char *buf;

    if (qemu_opts_validate(opts, smbios_type_0_desc) == -1) {
        return -1;
    }

    buf = qemu_opt_get(opts, "vendor");
    if (buf) {
        smbios_add_field(0, offsetof(struct smbios_type_0, vendor_str),
                         strlen(buf) + 1, buf);
    }
    buf = qemu_opt_get(opts, "version");
    if (buf) {
        smbios_add_field(0, offsetof(struct smbios_type_0, bios_version_str),
                         strlen(buf) + 1, buf);
    }
    buf = qemu_opt_get(opts, "date");
    if (buf) {
        smbios_add_field(0, offsetof(struct smbios_type_0,
                                     bios_release_date_str),
                                     strlen(buf) + 1, buf);
    }
    buf = qemu_opt_get(opts, "release");
    if (buf) {
        int major, minor;
        sscanf(buf, "%d.%d", &major, &minor);
        smbios_add_field(0, offsetof(struct smbios_type_0,
                                     system_bios_major_release), 1, &major);
        smbios_add_field(0, offsetof(struct smbios_type_0,
                                     system_bios_minor_release), 1, &minor);
    }
    return 0;
}

static QemuOptDesc smbios_type_1_desc[] = {
    {
        .name = "type",
        .type = QEMU_OPT_NUMBER,
    },
    {
        .name = "manufacturer",
        .type = QEMU_OPT_STRING,
    },
    {
        .name = "product",
        .type = QEMU_OPT_STRING,
    },
    {
        .name = "version",
        .type = QEMU_OPT_STRING,
    },
    {
        .name = "serial",
        .type = QEMU_OPT_STRING,
    },
    {
        .name = "uuid",
        .type = QEMU_OPT_STRING,
    },
    {
        .name = "sku",
        .type = QEMU_OPT_STRING,
    },
    {
        .name = "family",
        .type = QEMU_OPT_STRING,
    },
    { /* end of list */ }
};

static int smbios_build_type_1_fields(QemuOpts *opts)
{
    const char *buf;

    if (qemu_opts_validate(opts, smbios_type_1_desc) == -1) {
        return -1;
    }

    buf = qemu_opt_get(opts, "manufacturer");
    if (buf) {
        smbios_add_field(1, offsetof(struct smbios_type_1, manufacturer_str),
                         strlen(buf) + 1, buf);
    }
    buf = qemu_opt_get(opts, "product");
    if (buf) {
        smbios_add_field(1, offsetof(struct smbios_type_1, product_name_str),
                         strlen(buf) + 1, buf);
    }
    buf = qemu_opt_get(opts, "version");
    if (buf) {
        smbios_add_field(1, offsetof(struct smbios_type_1, version_str),
                         strlen(buf) + 1, buf);
    }
    buf = qemu_opt_get(opts, "serial");
    if (buf) {
        smbios_add_field(1, offsetof(struct smbios_type_1, serial_number_str),
                         strlen(buf) + 1, buf);
    }
    buf = qemu_opt_get(opts, "uuid");
    if (buf) {
        if (qemu_uuid_parse(buf, qemu_uuid) != 0) {
            fprintf(stderr, "Invalid SMBIOS UUID string\n");
            exit(1);
        }
        smbios_add_field(1, offsetof(struct smbios_type_1, uuid), 16, qemu_uuid);
    }
    buf = qemu_opt_get(opts, "sku");
    if (buf) {
        smbios_add_field(1, offsetof(struct smbios_type_1, sku_number_str),
                         strlen(buf) + 1, buf);
    }
    buf = qemu_opt_get(opts, "family");
    if (buf) {
        smbios_add_field(1, offsetof(struct smbios_type_1, family_str),
                         strlen(buf) + 1, buf);
    }
    return 0;
}

static QemuOptDesc smbios_file_desc[] = {
    {
        .name = "file",
        .type = QEMU_OPT_STRING,
    },
    { /* end of list */ }
};

int smbios_entry_add(QemuOpts *opts, void *opaque)
{
    const char *buf;

    buf = qemu_opt_get(opts, "file");
    if (buf) {
        struct smbios_structure_header *header;
        struct smbios_table *table;
        int size;

        if (qemu_opts_validate(opts, smbios_file_desc) == -1) {
            return -1;
        }

        size = get_image_size(buf);
        if (size == -1 || size < sizeof(struct smbios_structure_header)) {
            fprintf(stderr, "Cannot read smbios file %s\n", buf);
            exit(1);
        }

        if (!smbios_entries) {
            smbios_entries_len = sizeof(uint16_t);
            smbios_entries = g_malloc0(smbios_entries_len);
        }

        smbios_entries = g_realloc(smbios_entries, smbios_entries_len +
                                                      sizeof(*table) + size);
        table = (struct smbios_table *)(smbios_entries + smbios_entries_len);
        table->header.type = SMBIOS_TABLE_ENTRY;
        table->header.length = cpu_to_le16(sizeof(*table) + size);

        if (load_image(buf, table->data) != size) {
            fprintf(stderr, "Failed to load smbios file %s", buf);
            exit(1);
        }

        header = (struct smbios_structure_header *)(table->data);
        smbios_check_collision(header->type, SMBIOS_TABLE_ENTRY);
        if (header->type == 4) {
            smbios_type4_count++;
        }

        smbios_entries_len += sizeof(*table) + size;
        (*(uint16_t *)smbios_entries) =
                cpu_to_le16(le16_to_cpu(*(uint16_t *)smbios_entries) + 1);
        return 0;
    }

    buf = qemu_opt_get(opts, "type");
    if (buf != NULL) {
        unsigned long type = strtoul(buf, NULL, 0);
        switch (type) {
        case 0:
            return smbios_build_type_0_fields(opts);
        case 1:
            return smbios_build_type_1_fields(opts);
        default:
            fprintf(stderr, "Don't know how to build fields for SMBIOS type "
                    "%ld\n", type);
            exit(1);
        }
    }

    fprintf(stderr, "smbios: must specify type= or file=\n");
    return -1;
}
