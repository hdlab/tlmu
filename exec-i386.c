/*
 *  i386 emulator main execution loop
 * 
 *  Copyright (c) 2003 Fabrice Bellard
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include "exec-i386.h"

#define DEBUG_EXEC
#define DEBUG_FLUSH

/* main execution loop */

/* maximum total translate dcode allocated */
#define CODE_GEN_BUFFER_SIZE     (2048 * 1024)
//#define CODE_GEN_BUFFER_SIZE     (128 * 1024)
#define CODE_GEN_MAX_SIZE        65536
#define CODE_GEN_ALIGN           16 /* must be >= of the size of a icache line */

/* threshold to flush the translated code buffer */
#define CODE_GEN_BUFFER_MAX_SIZE (CODE_GEN_BUFFER_SIZE - CODE_GEN_MAX_SIZE)

#define CODE_GEN_MAX_BLOCKS    (CODE_GEN_BUFFER_SIZE / 64)
#define CODE_GEN_HASH_BITS     15
#define CODE_GEN_HASH_SIZE     (1 << CODE_GEN_HASH_BITS)
typedef struct TranslationBlock {
    unsigned long pc;   /* simulated PC corresponding to this block */
    uint8_t *tc_ptr;    /* pointer to the translated code */
    struct TranslationBlock *hash_next; /* next matching block */
} TranslationBlock;

TranslationBlock tbs[CODE_GEN_MAX_BLOCKS];
TranslationBlock *tb_hash[CODE_GEN_HASH_SIZE];
int nb_tbs;

uint8_t code_gen_buffer[CODE_GEN_BUFFER_SIZE];
uint8_t *code_gen_ptr;

#ifdef DEBUG_EXEC
static const char *cc_op_str[] = {
    "DYNAMIC",
    "EFLAGS",
    "MUL",
    "ADDB",
    "ADDW",
    "ADDL",
    "ADCB",
    "ADCW",
    "ADCL",
    "SUBB",
    "SUBW",
    "SUBL",
    "SBBB",
    "SBBW",
    "SBBL",
    "LOGICB",
    "LOGICW",
    "LOGICL",
    "INCB",
    "INCW",
    "INCL",
    "DECB",
    "DECW",
    "DECL",
    "SHLB",
    "SHLW",
    "SHLL",
    "SARB",
    "SARW",
    "SARL",
};

static void cpu_x86_dump_state(void)
{
    int eflags;
    eflags = cc_table[CC_OP].compute_all();
    eflags |= (DF & DIRECTION_FLAG);
    fprintf(logfile, 
            "EAX=%08x EBX=%08X ECX=%08x EDX=%08x\n"
            "ESI=%08x EDI=%08X EBP=%08x ESP=%08x\n"
            "CCS=%08x CCD=%08x CCO=%-8s EFL=%c%c%c%c%c%c%c\n",
            env->regs[R_EAX], env->regs[R_EBX], env->regs[R_ECX], env->regs[R_EDX], 
            env->regs[R_ESI], env->regs[R_EDI], env->regs[R_EBP], env->regs[R_ESP], 
            env->cc_src, env->cc_dst, cc_op_str[env->cc_op],
            eflags & DIRECTION_FLAG ? 'D' : '-',
            eflags & CC_O ? 'O' : '-',
            eflags & CC_S ? 'S' : '-',
            eflags & CC_Z ? 'Z' : '-',
            eflags & CC_A ? 'A' : '-',
            eflags & CC_P ? 'P' : '-',
            eflags & CC_C ? 'C' : '-'
            );
#if 1
    fprintf(logfile, "ST0=%f ST1=%f ST2=%f ST3=%f\n", 
            (double)ST0, (double)ST1, (double)ST(2), (double)ST(3));
#endif
}

#endif

void cpu_x86_tblocks_init(void)
{
    if (!code_gen_ptr) {
        code_gen_ptr = code_gen_buffer;
    }
}

/* flush all the translation blocks */
static void tb_flush(void)
{
    int i;
#ifdef DEBUG_FLUSH
    printf("gemu: flush code_size=%d nb_tbs=%d avg_tb_size=%d\n", 
           code_gen_ptr - code_gen_buffer, 
           nb_tbs, 
           (code_gen_ptr - code_gen_buffer) / nb_tbs);
#endif
    nb_tbs = 0;
    for(i = 0;i < CODE_GEN_HASH_SIZE; i++)
        tb_hash[i] = NULL;
    code_gen_ptr = code_gen_buffer;
    /* XXX: flush processor icache at this point */
}

/* find a translation block in the translation cache. If not found,
   allocate a new one */
static inline TranslationBlock *tb_find_and_alloc(unsigned long pc)
{
    TranslationBlock **ptb, *tb;
    unsigned int h;
 
    h = pc & (CODE_GEN_HASH_SIZE - 1);
    ptb = &tb_hash[h];
    for(;;) {
        tb = *ptb;
        if (!tb)
            break;
        if (tb->pc == pc)
            return tb;
        ptb = &tb->hash_next;
    }
    if (nb_tbs >= CODE_GEN_MAX_BLOCKS || 
        (code_gen_ptr - code_gen_buffer) >= CODE_GEN_BUFFER_MAX_SIZE)
        tb_flush();
    tb = &tbs[nb_tbs++];
    *ptb = tb;
    tb->pc = pc;
    tb->tc_ptr = NULL;
    tb->hash_next = NULL;
    return tb;
}

int cpu_x86_exec(CPUX86State *env1)
{
    int saved_T0, saved_T1, saved_A0;
    CPUX86State *saved_env;
    int code_gen_size, ret;
    void (*gen_func)(void);
    TranslationBlock *tb;
    uint8_t *tc_ptr;
    
    /* first we save global registers */
    saved_T0 = T0;
    saved_T1 = T1;
    saved_A0 = A0;
    saved_env = env;
    env = env1;
    
    /* prepare setjmp context for exception handling */
    if (setjmp(env->jmp_env) == 0) {
        for(;;) {
#ifdef DEBUG_EXEC
            if (loglevel) {
                cpu_x86_dump_state();
            }
#endif
            tb = tb_find_and_alloc((unsigned long)env->pc);
            tc_ptr = tb->tc_ptr;
            if (!tb->tc_ptr) {
                /* if no translated code available, then translate it now */
                tc_ptr = code_gen_ptr;
                cpu_x86_gen_code(code_gen_ptr, CODE_GEN_MAX_SIZE, 
                                 &code_gen_size, (uint8_t *)env->pc);
                tb->tc_ptr = tc_ptr;
                code_gen_ptr = (void *)(((unsigned long)code_gen_ptr + code_gen_size + CODE_GEN_ALIGN - 1) & ~(CODE_GEN_ALIGN - 1));
            }
            /* execute the generated code */
            gen_func = (void *)tc_ptr;
            gen_func();
        }
    }
    ret = env->exception_index;

    /* restore global registers */
    T0 = saved_T0;
    T1 = saved_T1;
    A0 = saved_A0;
    env = saved_env;
    return ret;
}
