/*
 * QEMU KVM support, paravirtual clock device
 *
 * Copyright (C) 2011 Siemens AG
 *
 * Authors:
 *  Jan Kiszka        <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL version 2.
 * See the COPYING file in the top-level directory.
 *
 */

#ifdef CONFIG_KVM

DeviceState *kvmclock_create(void);

#else /* CONFIG_KVM */

static inline DeviceState *kvmclock_create(void)
{
    return NULL;
}

#endif /* !CONFIG_KVM */
