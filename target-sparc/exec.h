#ifndef EXEC_SPARC_H
#define EXEC_SPARC_H 1
#include "dyngen-exec.h"

register struct CPUSPARCState *env asm(AREG0);
#ifdef TARGET_SPARC64
#define T0 (env->t0)
#define T1 (env->t1)
#define T2 (env->t2)
#else
register uint32_t T0 asm(AREG1);
register uint32_t T1 asm(AREG2);
register uint32_t T2 asm(AREG3);
#endif
#define FT0 (env->ft0)
#define FT1 (env->ft1)
#define FT2 (env->ft2)
#define DT0 (env->dt0)
#define DT1 (env->dt1)
#define DT2 (env->dt2)

#include "cpu.h"
#include "exec-all.h"

void cpu_lock(void);
void cpu_unlock(void);
void cpu_loop_exit(void);
void helper_flush(target_ulong addr);
void helper_ld_asi(int asi, int size, int sign);
void helper_st_asi(int asi, int size, int sign);
void helper_rett(void);
void helper_ldfsr(void);
void set_cwp(int new_cwp);
void do_fitos(void);
void do_fitod(void);
void do_fabss(void);
void do_fsqrts(void);
void do_fsqrtd(void);
void do_fcmps(void);
void do_fcmpd(void);
void do_ldd_kernel(target_ulong addr);
void do_ldd_user(target_ulong addr);
void do_ldd_raw(target_ulong addr);
void do_interrupt(int intno);
void raise_exception(int tt);
void memcpy32(target_ulong *dst, const target_ulong *src);
target_ulong mmu_probe(target_ulong address, int mmulev);
void dump_mmu(void);
void helper_debug();
void do_wrpsr();
void do_rdpsr();

/* XXX: move that to a generic header */
#if !defined(CONFIG_USER_ONLY)

#define ldul_user ldl_user
#define ldul_kernel ldl_kernel

#define ACCESS_TYPE 0
#define MEMSUFFIX _kernel
#define DATA_SIZE 1
#include "softmmu_header.h"

#define DATA_SIZE 2
#include "softmmu_header.h"

#define DATA_SIZE 4
#include "softmmu_header.h"

#define DATA_SIZE 8
#include "softmmu_header.h"
#undef ACCESS_TYPE
#undef MEMSUFFIX

#define ACCESS_TYPE 1
#define MEMSUFFIX _user
#define DATA_SIZE 1
#include "softmmu_header.h"

#define DATA_SIZE 2
#include "softmmu_header.h"

#define DATA_SIZE 4
#include "softmmu_header.h"

#define DATA_SIZE 8
#include "softmmu_header.h"
#undef ACCESS_TYPE
#undef MEMSUFFIX

/* these access are slower, they must be as rare as possible */
#define ACCESS_TYPE 2
#define MEMSUFFIX _data
#define DATA_SIZE 1
#include "softmmu_header.h"

#define DATA_SIZE 2
#include "softmmu_header.h"

#define DATA_SIZE 4
#include "softmmu_header.h"

#define DATA_SIZE 8
#include "softmmu_header.h"
#undef ACCESS_TYPE
#undef MEMSUFFIX

#define ldub(p) ldub_data(p)
#define ldsb(p) ldsb_data(p)
#define lduw(p) lduw_data(p)
#define ldsw(p) ldsw_data(p)
#define ldl(p) ldl_data(p)
#define ldq(p) ldq_data(p)

#define stb(p, v) stb_data(p, v)
#define stw(p, v) stw_data(p, v)
#define stl(p, v) stl_data(p, v)
#define stq(p, v) stq_data(p, v)

#endif /* !defined(CONFIG_USER_ONLY) */

static inline void env_to_regs(void)
{
}

static inline void regs_to_env(void)
{
}

int cpu_sparc_handle_mmu_fault(CPUState *env, target_ulong address, int rw,
                               int is_user, int is_softmmu);

#endif