/*
 * QTest testcase for the sPAPR LLAN
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
#include "qemu-common.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>

#define H_REGISTER_LOGICAL_LAN  0x114
#define H_FREE_LOGICAL_LAN      0x118
#define H_ADD_LOGICAL_LAN_BUFFER 0x11C
#define H_SEND_LOGICAL_LAN      0x120

#define KVMPPC_HCALL_BASE       0xf000
#define KVMPPC_H_RTAS           (KVMPPC_HCALL_BASE + 0x0)
#define KVMPPC_H_LOGICAL_MEMOP  (KVMPPC_HCALL_BASE + 0x1)
#define KVMPPC_HCALL_MAX        KVMPPC_H_LOGICAL_MEMOP

/* RTAS format:
   u32    token
   u32    nargs
   u32    nret
   u32    args[nargs]
   u32    rets[nret]
*/
static void rtas_call(const char *name,
                      uint32_t nargs, const uint32_t *args,
                      uint32_t nret, uint32_t *rets)
{
    uint64_t rtas_mem;
    uint32_t token;
    uint32_t words[3 + nargs + nret], *pword;
    int i;

    token = rtas_lookup(name);

    /* Why not... */
    rtas_mem = 0x00000000;

    pword = words;
    *pword++ = cpu_to_be32(token);
    *pword++ = cpu_to_be32(nargs);
    *pword++ = cpu_to_be32(nret);
    for (i = 0; i < nargs; i++) {
        *pword++ = cpu_to_be32(args[i]);
    }

    memwrite(rtas_mem, words, sizeof(words));
    spapr_hcall1(KVMPPC_H_RTAS, rtas_mem);
    memread(rtas_mem, words, sizeof(words));

    for (i = 0; i < nret; i++) {
        rets[i] = be32_to_cpu(*pword++);
    }
}

static uint32_t rtas_set_tce_bypass(uint32_t unit, uint32_t enable)
{
    uint32_t args[2] = { unit, enable };
    uint32_t ret;

    rtas_call("ibm,set-tce-bypass", 2, args, 1, &ret);

    return ret;
}

#define SPAPR_TCE_PAGE_SHIFT   12
#define SPAPR_TCE_PAGE_SIZE    (1ULL << SPAPR_TCE_PAGE_SHIFT)
#define SPAPR_TCE_PAGE_MASK    (SPAPR_TCE_PAGE_SIZE - 1)

#define VLAN_BD_VALID        0x8000000000000000ULL
#define VLAN_BD_TOGGLE       0x4000000000000000ULL
#define VLAN_BD_NO_CSUM      0x0200000000000000ULL
#define VLAN_BD_CSUM_GOOD    0x0100000000000000ULL
#define VLAN_BD_LEN_MASK     0x00ffffff00000000ULL
#define VLAN_BD_ADDR_MASK    0x00000000ffffffffULL

static inline uint32_t vlan_bd_len(uint64_t bd)
{
    return (bd & VLAN_BD_LEN_MASK) >> 32;
}

static inline uint32_t vlan_bd_addr(uint64_t bd)
{
    return bd & VLAN_BD_ADDR_MASK;
}

static uint64_t vlan_bd_make(uint32_t addr, uint32_t len)
{
    return (((uint64_t)len << 32) & VLAN_BD_LEN_MASK) | 
        (addr & VLAN_BD_ADDR_MASK);
}

static void llan_overflow(void)
{
    uint32_t reg = 0x71000004;
    uint64_t ret;
    uint64_t buf_list, filter_list, rec_queue;
    uint32_t offset;
    uint64_t packet;
    uint8_t packet_data[] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x07,
        0x02,
        0x42, 0x42
    };

    /* First page is for RTAS */
    offset = SPAPR_TCE_PAGE_SIZE;

    /* One page for buf list */
    buf_list = offset;
    offset += SPAPR_TCE_PAGE_SIZE;

    /* One page for filter list */
    filter_list = offset;
    offset += SPAPR_TCE_PAGE_SIZE;

    /* Setup receive queue as a full page */
    rec_queue = vlan_bd_make(offset, SPAPR_TCE_PAGE_SIZE);
    rec_queue |= VLAN_BD_VALID;

    offset += SPAPR_TCE_PAGE_SIZE;

    rtas_set_tce_bypass(reg, true);

    ret = spapr_hcall4(H_REGISTER_LOGICAL_LAN, reg,
                       buf_list, rec_queue, filter_list);

    g_assert_cmpint(ret, ==, 0);

    packet = offset;
    offset += SPAPR_TCE_PAGE_SIZE;
    memwrite(packet, packet_data, sizeof(packet_data));

    packet = vlan_bd_make(packet, sizeof(packet_data));
    packet |= VLAN_BD_VALID;

    ret = spapr_hcall2(H_SEND_LOGICAL_LAN, reg, packet);
    g_assert_cmpint(ret, ==, 0);

    ret = spapr_hcall2(H_SEND_LOGICAL_LAN, reg, packet);
    g_assert_cmpint(ret, ==, 0);
}

int main(int argc, char **argv)
{
    int ret;
    const char *sock = "/tmp/qtest-sock.sock";
    struct sockaddr_un addr;
    int s;

    g_test_init(&argc, &argv, NULL);

    s = socket(PF_UNIX, SOCK_STREAM, 0);
    g_assert(s != -1);

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);

    unlink(sock);
    ret = bind(s, (struct sockaddr *)&addr. sizeof(addr));
    g_assert(s != -1);

    ret = listen(s, 1);
    g_assert(ret != -1)

    qtest_start("-display none -device spapr-vlan,netdev=net0,reg=0x71000004 "
                "-machine pseries -netdev socket,path=id=net0");

    qtest_add_func("/llan/overflow", llan_overflow);
    ret = g_test_run();

    qtest_quit(global_qtest);

    return ret;
}
