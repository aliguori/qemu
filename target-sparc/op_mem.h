/***                             Integer load                              ***/
#define SPARC_LD_OP(name, qp)                                                 \
void OPPROTO glue(glue(op_, name), MEMSUFFIX)(void)                           \
{                                                                             \
    T1 = glue(qp, MEMSUFFIX)(T0);                                     \
}

#define SPARC_ST_OP(name, op)                                                 \
void OPPROTO glue(glue(op_, name), MEMSUFFIX)(void)                           \
{                                                                             \
    glue(op, MEMSUFFIX)(T0, T1);                                      \
}

SPARC_LD_OP(ld, ldl);
SPARC_LD_OP(ldub, ldub);
SPARC_LD_OP(lduh, lduw);
SPARC_LD_OP(ldsb, ldsb);
SPARC_LD_OP(ldsh, ldsw);

/***                              Integer store                            ***/
SPARC_ST_OP(st, stl);
SPARC_ST_OP(stb, stb);
SPARC_ST_OP(sth, stw);

void OPPROTO glue(op_std, MEMSUFFIX)(void)
{
    glue(stl, MEMSUFFIX)(T0, T1);
    glue(stl, MEMSUFFIX)((T0 + 4), T2);
}

void OPPROTO glue(op_ldstub, MEMSUFFIX)(void)
{
    T1 = glue(ldub, MEMSUFFIX)(T0);
    glue(stb, MEMSUFFIX)(T0, 0xff);     /* XXX: Should be Atomically */
}

void OPPROTO glue(op_swap, MEMSUFFIX)(void)
{
    target_ulong tmp = glue(ldl, MEMSUFFIX)(T0);
    glue(stl, MEMSUFFIX)(T0, T1);       /* XXX: Should be Atomically */
    T1 = tmp;
}

void OPPROTO glue(op_ldd, MEMSUFFIX)(void)
{
    T1 = glue(ldl, MEMSUFFIX)(T0);
    T0 = glue(ldl, MEMSUFFIX)((T0 + 4));
}

/***                         Floating-point store                          ***/
void OPPROTO glue(op_stf, MEMSUFFIX) (void)
{
    glue(stfl, MEMSUFFIX)(T0, FT0);
}

void OPPROTO glue(op_stdf, MEMSUFFIX) (void)
{
    glue(stfq, MEMSUFFIX)(T0, DT0);
}

/***                         Floating-point load                           ***/
void OPPROTO glue(op_ldf, MEMSUFFIX) (void)
{
    FT0 = glue(ldfl, MEMSUFFIX)(T0);
}

void OPPROTO glue(op_lddf, MEMSUFFIX) (void)
{
    DT0 = glue(ldfq, MEMSUFFIX)(T0);
}
#undef MEMSUFFIX