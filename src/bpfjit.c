/*-
 * Copyright (c) 2011-2014 Alexander Nasonov.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "bpfjit.h"

#include <sys/types.h>
#include <sys/queue.h>

#ifndef _KERNEL
#include <stdlib.h>
#include <assert.h>
#define BJ_ALLOC(sz) malloc(sz)
#define BJ_FREE(p, sz) free(p)
#define BJ_ASSERT(c) assert(c)
#else
#include <sys/kmem.h>
#define BJ_ALLOC(sz) kmem_alloc(sz, KM_SLEEP)
#define BJ_FREE(p, sz) kmem_free(p, sz)
#define BJ_ASSERT(c) KASSERT(c)
#endif

#ifndef _KERNEL
#include <limits.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#else
#include <sys/atomic.h>
#include <sys/module.h>
#endif

#include <sljitLir.h>

/*
 * Arguments of generated bpfjit_function_t.
 * The first argument is reassigned upon entry
 * to a more frequently used buf argument.
 */
#define BJ_CTX_ARG	SLJIT_SAVED_REG1
#define BJ_ARGS		SLJIT_SAVED_REG2

/*
 * Permanent register assignments.
 */
#define BJ_BUF		SLJIT_SAVED_REG1
//#define BJ_ARGS  	SLJIT_SAVED_REG2
#define BJ_BUFLEN	SLJIT_SAVED_REG3
#define BJ_AREG		SLJIT_SCRATCH_REG1
#define BJ_TMP1REG	SLJIT_SCRATCH_REG2
#define BJ_TMP2REG	SLJIT_SCRATCH_REG3
#define BJ_XREG		SLJIT_TEMPORARY_EREG1
#define BJ_TMP3REG	SLJIT_TEMPORARY_EREG2

/*
 * EREG registers can't be used for indirect calls, reuse BJ_BUF and
 * BJ_BUFLEN registers. They can be easily restored from BJ_ARGS.
 */
#define BJ_COPF_PTR	SLJIT_SAVED_REG1
#define BJ_COPF_IDX	SLJIT_SAVED_REG3

typedef unsigned int bpfjit_init_mask_t;
#define BJ_INIT_NOBITS  0u
#define BJ_INIT_MBIT(k) (1u << (k))
#define BJ_INIT_MMASK   (BJ_INIT_MBIT(BPF_MEMWORDS) - 1u)
#define BJ_INIT_ABIT    BJ_INIT_MBIT(BPF_MEMWORDS)
#define BJ_INIT_XBIT    BJ_INIT_MBIT(BPF_MEMWORDS + 1)

struct bpfjit_stack
{
	bpf_state_t state; // must be at offset 0
	bpf_ctx_t *ctx;
#ifdef _KERNEL
	void *tmp;
#endif
};

/*
 * Node of bj_jumps list.
 */
struct bpfjit_jump {
	struct sljit_jump *bj_jump;
	SLIST_ENTRY(bpfjit_jump) bj_entries;
	uint32_t bj_safe_length;
};

/*
 * Data for BPF_JMP instruction.
 */
struct bpfjit_jump_data {
	/*
	 * These entries make up bj_jumps list:
	 * bj_jtf[0] - when coming from jt path,
	 * bj_jtf[1] - when coming from jf path.
	 */
	struct bpfjit_jump bj_jtf[2];
};

/*
 * Data for "read from packet" instructions.
 * See also read_pkt_insn() function below.
 */
struct bpfjit_read_pkt_data {
	/*
	 * If positive, emit "if (buflen < bj_check_length) return 0".
	 * We assume that buflen is never equal to UINT32_MAX (otherwise,
	 * we'd need a special bool variable to emit unconditional "return 0").
	 */
	uint32_t bj_check_length;
};

/*
 * Additional (optimization-related) data for bpf_insn.
 */
struct bpfjit_insn_data {
	/* List of jumps to this insn. */
	SLIST_HEAD(, bpfjit_jump) bj_jumps;

	union {
		struct bpfjit_jump_data     bj_jdata;
		struct bpfjit_read_pkt_data bj_rdata;
	} bj_aux;

	bpfjit_init_mask_t bj_invalid;
	bool bj_unreachable;
};

#ifdef _KERNEL

uint32_t m_xword(const struct mbuf *, uint32_t, int *);
uint32_t m_xhalf(const struct mbuf *, uint32_t, int *);
uint32_t m_xbyte(const struct mbuf *, uint32_t, int *);

MODULE(MODULE_CLASS_MISC, bpfjit, "sljit")

static int
bpfjit_modcmd(modcmd_t cmd, void *arg)
{

	switch (cmd) {
	case MODULE_CMD_INIT:
		bpfjit_module_ops.bj_free_code = &bpfjit_free_code;
		membar_producer();
		bpfjit_module_ops.bj_generate_code = &bpfjit_generate_code;
		membar_producer();
		return 0;

	case MODULE_CMD_FINI:
		return EOPNOTSUPP;

	default:
		return ENOTTY;
	}
}
#endif

static uint32_t
read_width(struct bpf_insn *pc)
{

	switch (BPF_SIZE(pc->code)) {
	case BPF_W:
		return 4;
	case BPF_H:
		return 2;
	case BPF_B:
		return 1;
	default:
		BJ_ASSERT(false);
		return 0;
	}
}

/*
 * Copy buf and buflen members of bpf_args from BJ_ARGS
 * pointer to BJ_BUF and BJ_BUFLEN registers.
 */
static int
load_buf_buflen(struct sljit_compiler *compiler)
{
	int status;

	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_P,
	    BJ_BUF, 0,
	    SLJIT_MEM1(BJ_ARGS),
	    offsetof(struct bpf_args, pkt));
	if (status != SLJIT_SUCCESS)
		return status;

	status = sljit_emit_op1(compiler,
	    SLJIT_MOV,
	    BJ_BUFLEN, 0,
	    SLJIT_MEM1(BJ_ARGS),
	    offsetof(struct bpf_args, buflen));

	return status;
}

static bool
grow_jumps(struct sljit_jump ***jumps, size_t *size)
{
	struct sljit_jump **newptr;
	const size_t elemsz = sizeof(struct sljit_jump *);
	size_t old_size = *size;
	size_t new_size = 2 * old_size;

	if (new_size < old_size || new_size > SIZE_MAX / elemsz)
		return false;

	newptr = BJ_ALLOC(new_size * elemsz);
	if (newptr == NULL)
		return false;

	memcpy(newptr, *jumps, old_size * elemsz);
	BJ_FREE(*jumps, old_size * elemsz);

	*jumps = newptr;
	*size = new_size;
	return true;
}

static bool
append_jump(struct sljit_jump *jump, struct sljit_jump ***jumps,
    size_t *size, size_t *max_size)
{
	if (*size == *max_size && !grow_jumps(jumps, max_size))
		return false;

	(*jumps)[(*size)++] = jump;
	return true;
}

/*
 * Generate code for BPF_LD+BPF_B+BPF_ABS    A <- P[k:1].
 */
static int
emit_read8(struct sljit_compiler* compiler, uint32_t k)
{

	return sljit_emit_op1(compiler,
	    SLJIT_MOV_UB,
	    BJ_AREG, 0,
	    SLJIT_MEM1(BJ_BUF), k);
}

/*
 * Generate code for BPF_LD+BPF_H+BPF_ABS    A <- P[k:2].
 */
static int
emit_read16(struct sljit_compiler* compiler, uint32_t k)
{
	int status;

	/* tmp1 = buf[k]; */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_UB,
	    BJ_TMP1REG, 0,
	    SLJIT_MEM1(BJ_BUF), k);
	if (status != SLJIT_SUCCESS)
		return status;

	/* A = buf[k+1]; */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_UB,
	    BJ_AREG, 0,
	    SLJIT_MEM1(BJ_BUF), k+1);
	if (status != SLJIT_SUCCESS)
		return status;

	/* tmp1 = tmp1 << 8; */
	status = sljit_emit_op2(compiler,
	    SLJIT_SHL,
	    BJ_TMP1REG, 0,
	    BJ_TMP1REG, 0,
	    SLJIT_IMM, 8);
	if (status != SLJIT_SUCCESS)
		return status;

	/* A = A + tmp1; */
	status = sljit_emit_op2(compiler,
	    SLJIT_ADD,
	    BJ_AREG, 0,
	    BJ_AREG, 0,
	    BJ_TMP1REG, 0);
	return status;
}

/*
 * Generate code for BPF_LD+BPF_W+BPF_ABS    A <- P[k:4].
 */
static int
emit_read32(struct sljit_compiler* compiler, uint32_t k)
{
	int status;

	/* tmp1 = buf[k]; */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_UB,
	    BJ_TMP1REG, 0,
	    SLJIT_MEM1(BJ_BUF), k);
	if (status != SLJIT_SUCCESS)
		return status;

	/* tmp2 = buf[k+1]; */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_UB,
	    BJ_TMP2REG, 0,
	    SLJIT_MEM1(BJ_BUF), k+1);
	if (status != SLJIT_SUCCESS)
		return status;

	/* A = buf[k+3]; */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_UB,
	    BJ_AREG, 0,
	    SLJIT_MEM1(BJ_BUF), k+3);
	if (status != SLJIT_SUCCESS)
		return status;

	/* tmp1 = tmp1 << 24; */
	status = sljit_emit_op2(compiler,
	    SLJIT_SHL,
	    BJ_TMP1REG, 0,
	    BJ_TMP1REG, 0,
	    SLJIT_IMM, 24);
	if (status != SLJIT_SUCCESS)
		return status;

	/* A = A + tmp1; */
	status = sljit_emit_op2(compiler,
	    SLJIT_ADD,
	    BJ_AREG, 0,
	    BJ_AREG, 0,
	    BJ_TMP1REG, 0);
	if (status != SLJIT_SUCCESS)
		return status;

	/* tmp1 = buf[k+2]; */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_UB,
	    BJ_TMP1REG, 0,
	    SLJIT_MEM1(BJ_BUF), k+2);
	if (status != SLJIT_SUCCESS)
		return status;

	/* tmp2 = tmp2 << 16; */
	status = sljit_emit_op2(compiler,
	    SLJIT_SHL,
	    BJ_TMP2REG, 0,
	    BJ_TMP2REG, 0,
	    SLJIT_IMM, 16);
	if (status != SLJIT_SUCCESS)
		return status;

	/* A = A + tmp2; */
	status = sljit_emit_op2(compiler,
	    SLJIT_ADD,
	    BJ_AREG, 0,
	    BJ_AREG, 0,
	    BJ_TMP2REG, 0);
	if (status != SLJIT_SUCCESS)
		return status;

	/* tmp1 = tmp1 << 8; */
	status = sljit_emit_op2(compiler,
	    SLJIT_SHL,
	    BJ_TMP1REG, 0,
	    BJ_TMP1REG, 0,
	    SLJIT_IMM, 8);
	if (status != SLJIT_SUCCESS)
		return status;

	/* A = A + tmp1; */
	status = sljit_emit_op2(compiler,
	    SLJIT_ADD,
	    BJ_AREG, 0,
	    BJ_AREG, 0,
	    BJ_TMP1REG, 0);
	return status;
}

#ifdef _KERNEL
/*
 * Generate m_xword/m_xhalf/m_xbyte call.
 *
 * pc is one of:
 * BPF_LD+BPF_W+BPF_ABS    A <- P[k:4]
 * BPF_LD+BPF_H+BPF_ABS    A <- P[k:2]
 * BPF_LD+BPF_B+BPF_ABS    A <- P[k:1]
 * BPF_LD+BPF_W+BPF_IND    A <- P[X+k:4]
 * BPF_LD+BPF_H+BPF_IND    A <- P[X+k:2]
 * BPF_LD+BPF_B+BPF_IND    A <- P[X+k:1]
 * BPF_LDX+BPF_B+BPF_MSH   X <- 4*(P[k:1]&0xf)
 *
 * The dst variable should be
 *  - BJ_AREG when emitting code for BPF_LD instructions,
 *  - BJ_XREG or any of BJ_TMP[1-3]REG registers when emitting
 *    code for BPF_MSH instruction.
 */
static int
emit_xcall(struct sljit_compiler* compiler, struct bpf_insn *pc,
    int dst, sljit_sw dstw, struct sljit_jump **ret0_jump,
    uint32_t (*fn)(const struct mbuf *, uint32_t, int *))
{
#if BJ_XREG == SLJIT_RETURN_REG   || \
    BJ_XREG == SLJIT_SCRATCH_REG1 || \
    BJ_XREG == SLJIT_SCRATCH_REG2 || \
    BJ_XREG == SLJIT_SCRATCH_REG3 || \
#error "Not supported assignment of registers."
#endif
	int status;

	/*
	 * The third argument of fn is an address on stack.
	 */
	const int arg3_offset = offsetof(struct bpfjit_stack, tmp);

	if (BPF_CLASS(pc->code) == BPF_LDX) {
		/* save A */
		status = sljit_emit_op1(compiler,
		    SLJIT_MOV,
		    BJ_TMP3REG, 0,
		    BJ_AREG, 0);
		if (status != SLJIT_SUCCESS)
			return status;
	}

	/*
	 * Prepare registers for fn(buf, k, &err) call.
	 */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV,
	    SLJIT_SCRATCH_REG1, 0,
	    BJ_BUF, 0);
	if (status != SLJIT_SUCCESS)
		return status;

	if (BPF_CLASS(pc->code) == BPF_LD && BPF_MODE(pc->code) == BPF_IND) {
		status = sljit_emit_op2(compiler,
		    SLJIT_ADD,
		    SLJIT_SCRATCH_REG2, 0,
		    BJ_XREG, 0,
		    SLJIT_IMM, (uint32_t)pc->k);
	} else {
		status = sljit_emit_op1(compiler,
		    SLJIT_MOV,
		    SLJIT_SCRATCH_REG2, 0,
		    SLJIT_IMM, (uint32_t)pc->k);
	}

	if (status != SLJIT_SUCCESS)
		return status;

	status = sljit_get_local_base(compiler,
	    SLJIT_SCRATCH_REG3, 0, arg3_offset);
	if (status != SLJIT_SUCCESS)
		return status;

	/* fn(buf, k, &err); */
	status = sljit_emit_ijump(compiler,
	    SLJIT_CALL3,
	    SLJIT_IMM, SLJIT_FUNC_OFFSET(fn));

	if (BPF_CLASS(pc->code) == BPF_LDX || dst != SLJIT_RETURN_REG) {
		/* move return value to dst */
		BJ_ASSERT(dst != SLJIT_RETURN_REG);
		status = sljit_emit_op1(compiler,
		    SLJIT_MOV,
		    dst, dstw,
		    SLJIT_RETURN_REG, 0);
		if (status != SLJIT_SUCCESS)
			return status;
	}

	if (BPF_CLASS(pc->code) == BPF_LDX) {
		/* restore A */
		status = sljit_emit_op1(compiler,
		    SLJIT_MOV,
		    BJ_AREG, 0,
		    BJ_TMP3REG, 0);
		if (status != SLJIT_SUCCESS)
			return status;
	}

	/* tmp3 = *err; */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_UI,
	    SLJIT_SCRATCH_REG3, 0,
	    SLJIT_MEM1(SLJIT_LOCALS_REG), arg3_offset);
	if (status != SLJIT_SUCCESS)
		return status;

	/* if (tmp3 != 0) return 0; */
	*ret0_jump = sljit_emit_cmp(compiler,
	    SLJIT_C_NOT_EQUAL,
	    SLJIT_SCRATCH_REG3, 0,
	    SLJIT_IMM, 0);
	if (*ret0_jump == NULL)
		return SLJIT_ERR_ALLOC_FAILED;

	return status;
}
#endif

/*
 * Emit code for BPF_COP and BPF_COPX instructions.
 */
static int
emit_cop(struct sljit_compiler* compiler, bpf_ctx_t *bc, struct bpf_insn *pc,
    struct sljit_jump **ret0_jump)
{
#if BJ_XREG == SLJIT_RETURN_REG   || \
    BJ_XREG == SLJIT_SCRATCH_REG1 || \
    BJ_XREG == SLJIT_SCRATCH_REG2 || \
    BJ_XREG == SLJIT_SCRATCH_REG3 || \
    BJ_COPF_PTR == BJ_ARGS        || \
    BJ_COPF_IDX	== BJ_ARGS
#error "Not supported assignment of registers."
#endif

	struct sljit_jump *jump;
	int status;
	bool skip;

	jump = NULL;
	skip = false;

	if (bc == NULL) {
		skip = true;
		jump = sljit_emit_jump(compiler, SLJIT_JUMP);
		if (jump == NULL)
			return SLJIT_ERR_ALLOC_FAILED;
	}

	if (BPF_MISCOP(pc->code) == BPF_COP && !skip && pc->k >= bc->nfuncs) {
		skip = true;
		jump = sljit_emit_jump(compiler, SLJIT_JUMP);
		if (jump == NULL)
			return SLJIT_ERR_ALLOC_FAILED;
	} else if (BPF_MISCOP(pc->code) == BPF_COPX && !skip) {
		/* if (X >= bc->nfuncs) return 0; */
		jump = sljit_emit_cmp(compiler,
		    SLJIT_C_GREATER_EQUAL,
		    BJ_XREG, 0,
		    SLJIT_IMM, bc->nfuncs);
		if (jump == NULL)
			return SLJIT_ERR_ALLOC_FAILED;
	}

	if (jump != NULL)
		*ret0_jump = jump;

	if (skip)
		return SLJIT_SUCCESS;

	/* Copy A to bpf_state object. */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_P,
	    SLJIT_MEM1(SLJIT_LOCALS_REG),
	    offsetof(struct bpf_state, regA),
	    BJ_AREG, 0);
	if (status != SLJIT_SUCCESS)
		return status;

	/*
	 * Copy bpf_copfunc_t arguments to registers.
	 */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_P,
	    SLJIT_SCRATCH_REG1, 0,
	    SLJIT_MEM1(SLJIT_LOCALS_REG),
	    offsetof(struct bpfjit_stack, ctx));
	if (status != SLJIT_SUCCESS)
		return status;

	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_P,
	    SLJIT_SCRATCH_REG2, 0,
	    BJ_ARGS, 0);
	if (status != SLJIT_SUCCESS)
		return status;

	status = sljit_get_local_base(compiler,
	    SLJIT_SCRATCH_REG3, 0,
	    offsetof(struct bpfjit_stack, state));
	if (status != SLJIT_SUCCESS)
		return status;

	if (BPF_MISCOP(pc->code) == BPF_COP) {
		status = sljit_emit_ijump(compiler,
		    SLJIT_CALL3,
		    SLJIT_IMM, SLJIT_FUNC_OFFSET(bc->copfuncs[pc->k]));
		if (status != SLJIT_SUCCESS)
			return status;
	} else if (BPF_MISCOP(pc->code) == BPF_COPX) {
		/* load ctx->copfuncs */
		status = sljit_emit_op1(compiler,
		    SLJIT_MOV_P,
		    BJ_COPF_PTR, 0,
		    SLJIT_MEM1(SLJIT_SCRATCH_REG1),
		    offsetof(struct bpf_ctx, copfuncs));
		if (status != SLJIT_SUCCESS)
			return status;

		/* 
		 * Load X to a register that can be used for
		 * memory addressing.
		 */
		status = sljit_emit_op1(compiler,
		    SLJIT_MOV_P,
		    BJ_COPF_IDX, 0,
		    BJ_XREG, 0);
		if (status != SLJIT_SUCCESS)
			return status;

		status = sljit_emit_ijump(compiler,
		    SLJIT_CALL3,
		    SLJIT_MEM2(BJ_COPF_PTR, BJ_COPF_IDX),
		    SLJIT_WORD_SHIFT);
		if (status != SLJIT_SUCCESS)
			return status;

		status = load_buf_buflen(compiler);
		if (status != SLJIT_SUCCESS)
			return status;
	}

#if BJ_AREG != SLJIT_RETURN_REG
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV,
	    BJ_AREG, 0,
	    SLJIT_RETURN_REG, 0);
	if (status != SLJIT_SUCCESS)
		return status;
#endif

	return status;
}

/*
 * Generate code for
 * BPF_LD+BPF_W+BPF_ABS    A <- P[k:4]
 * BPF_LD+BPF_H+BPF_ABS    A <- P[k:2]
 * BPF_LD+BPF_B+BPF_ABS    A <- P[k:1]
 * BPF_LD+BPF_W+BPF_IND    A <- P[X+k:4]
 * BPF_LD+BPF_H+BPF_IND    A <- P[X+k:2]
 * BPF_LD+BPF_B+BPF_IND    A <- P[X+k:1]
 */
static int
emit_pkt_read(struct sljit_compiler* compiler,
    struct bpf_insn *pc, struct sljit_jump *to_mchain_jump,
    struct sljit_jump ***ret0, size_t *ret0_size, size_t *ret0_maxsize)
{
	int status;
	uint32_t width;
	struct sljit_jump *jump;
#ifdef _KERNEL
	struct sljit_label *label;
	struct sljit_jump *over_mchain_jump;
	const bool check_zero_buflen = (to_mchain_jump != NULL);
#endif
	const uint32_t k = pc->k;

#ifdef _KERNEL
	if (to_mchain_jump == NULL) {
		to_mchain_jump = sljit_emit_cmp(compiler,
		    SLJIT_C_EQUAL,
		    BJ_BUFLEN, 0,
		    SLJIT_IMM, 0);
		if (to_mchain_jump == NULL)
			return SLJIT_ERR_ALLOC_FAILED;
	}
#endif

	width = read_width(pc);

	if (BPF_MODE(pc->code) == BPF_IND) {
		/* tmp1 = buflen - (pc->k + width); */
		status = sljit_emit_op2(compiler,
		    SLJIT_SUB,
		    BJ_TMP1REG, 0,
		    BJ_BUFLEN, 0,
		    SLJIT_IMM, k + width);
		if (status != SLJIT_SUCCESS)
			return status;

		/* buf += X; */
		status = sljit_emit_op2(compiler,
		    SLJIT_ADD,
		    BJ_BUF, 0,
		    BJ_BUF, 0,
		    BJ_XREG, 0);
		if (status != SLJIT_SUCCESS)
			return status;

		/* if (tmp1 < X) return 0; */
		jump = sljit_emit_cmp(compiler,
		    SLJIT_C_LESS,
		    BJ_TMP1REG, 0,
		    BJ_XREG, 0);
		if (jump == NULL)
			return SLJIT_ERR_ALLOC_FAILED;
		if (!append_jump(jump, ret0, ret0_size, ret0_maxsize))
			return SLJIT_ERR_ALLOC_FAILED;
	}

	switch (width) {
	case 4:
		status = emit_read32(compiler, k);
		break;
	case 2:
		status = emit_read16(compiler, k);
		break;
	case 1:
		status = emit_read8(compiler, k);
		break;
	}

	if (status != SLJIT_SUCCESS)
		return status;

	if (BPF_MODE(pc->code) == BPF_IND) {
		/* buf -= X; */
		status = sljit_emit_op2(compiler,
		    SLJIT_SUB,
		    BJ_BUF, 0,
		    BJ_BUF, 0,
		    BJ_XREG, 0);
		if (status != SLJIT_SUCCESS)
			return status;
	}

#ifdef _KERNEL
	over_mchain_jump = sljit_emit_jump(compiler, SLJIT_JUMP);
	if (over_mchain_jump == NULL)
		return SLJIT_ERR_ALLOC_FAILED;

	/* entry point to mchain handler */
	label = sljit_emit_label(compiler);
	if (label == NULL)
		return SLJIT_ERR_ALLOC_FAILED;
	sljit_set_label(to_mchain_jump, label);

	if (check_zero_buflen) {
		/* if (buflen != 0) return 0; */
		jump = sljit_emit_cmp(compiler,
		    SLJIT_C_NOT_EQUAL,
		    BJ_BUFLEN, 0,
		    SLJIT_IMM, 0);
		if (jump == NULL)
			return SLJIT_ERR_ALLOC_FAILED;
		if (!append_jump(jump, ret0, ret0_size, ret0_maxsize))
			return SLJIT_ERR_ALLOC_FAILED;
	}

	switch (width) {
	case 4:
		status = emit_xcall(compiler, pc, BJ_AREG, 0, &jump, &m_xword);
		break;
	case 2:
		status = emit_xcall(compiler, pc, BJ_AREG, 0, &jump, &m_xhalf);
		break;
	case 1:
		status = emit_xcall(compiler, pc, BJ_AREG, 0, &jump, &m_xbyte);
		break;
	}

	if (status != SLJIT_SUCCESS)
		return status;

	if (!append_jump(jump, ret0, ret0_size, ret0_maxsize))
		return SLJIT_ERR_ALLOC_FAILED;

	label = sljit_emit_label(compiler);
	if (label == NULL)
		return SLJIT_ERR_ALLOC_FAILED;
	sljit_set_label(over_mchain_jump, label);
#endif

	return status;
}

/*
 * Generate code for BPF_LDX+BPF_B+BPF_MSH    X <- 4*(P[k:1]&0xf).
 */
static int
emit_msh(struct sljit_compiler* compiler,
    struct bpf_insn *pc, struct sljit_jump *to_mchain_jump,
    struct sljit_jump ***ret0, size_t *ret0_size, size_t *ret0_maxsize)
{
	int status;
#ifdef _KERNEL
	struct sljit_label *label;
	struct sljit_jump *jump, *over_mchain_jump;
	const bool check_zero_buflen = (to_mchain_jump != NULL);
#endif
	const uint32_t k = pc->k;

#ifdef _KERNEL
	if (to_mchain_jump == NULL) {
		to_mchain_jump = sljit_emit_cmp(compiler,
		    SLJIT_C_EQUAL,
		    BJ_BUFLEN, 0,
		    SLJIT_IMM, 0);
		if (to_mchain_jump == NULL)
			return SLJIT_ERR_ALLOC_FAILED;
	}
#endif

	/* tmp1 = buf[k] */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV_UB,
	    BJ_TMP1REG, 0,
	    SLJIT_MEM1(BJ_BUF), k);
	if (status != SLJIT_SUCCESS)
		return status;

	/* tmp1 &= 0xf */
	status = sljit_emit_op2(compiler,
	    SLJIT_AND,
	    BJ_TMP1REG, 0,
	    BJ_TMP1REG, 0,
	    SLJIT_IMM, 0xf);
	if (status != SLJIT_SUCCESS)
		return status;

	/* tmp1 = tmp1 << 2 */
	status = sljit_emit_op2(compiler,
	    SLJIT_SHL,
	    BJ_XREG, 0,
	    BJ_TMP1REG, 0,
	    SLJIT_IMM, 2);
	if (status != SLJIT_SUCCESS)
		return status;

#ifdef _KERNEL
	over_mchain_jump = sljit_emit_jump(compiler, SLJIT_JUMP);
	if (over_mchain_jump == NULL)
		return SLJIT_ERR_ALLOC_FAILED;

	/* entry point to mchain handler */
	label = sljit_emit_label(compiler);
	if (label == NULL)
		return SLJIT_ERR_ALLOC_FAILED;
	sljit_set_label(to_mchain_jump, label);

	if (check_zero_buflen) {
		/* if (buflen != 0) return 0; */
		jump = sljit_emit_cmp(compiler,
		    SLJIT_C_NOT_EQUAL,
		    BJ_BUFLEN, 0,
		    SLJIT_IMM, 0);
		if (jump == NULL)
			return SLJIT_ERR_ALLOC_FAILED;
		if (!append_jump(jump, ret0, ret0_size, ret0_maxsize))
			return SLJIT_ERR_ALLOC_FAILED;
	}

	status = emit_xcall(compiler, pc, BJ_TMP1REG, 0, &jump, &m_xbyte);
	if (status != SLJIT_SUCCESS)
		return status;

	if (!append_jump(jump, ret0, ret0_size, ret0_maxsize))
		return SLJIT_ERR_ALLOC_FAILED;

	/* tmp1 &= 0xf */
	status = sljit_emit_op2(compiler,
	    SLJIT_AND,
	    BJ_TMP1REG, 0,
	    BJ_TMP1REG, 0,
	    SLJIT_IMM, 0xf);
	if (status != SLJIT_SUCCESS)
		return status;

	/* tmp1 = tmp1 << 2 */
	status = sljit_emit_op2(compiler,
	    SLJIT_SHL,
	    BJ_XREG, 0,
	    BJ_TMP1REG, 0,
	    SLJIT_IMM, 2);
	if (status != SLJIT_SUCCESS)
		return status;


	label = sljit_emit_label(compiler);
	if (label == NULL)
		return SLJIT_ERR_ALLOC_FAILED;
	sljit_set_label(over_mchain_jump, label);
#endif

	return status;
}

static int
emit_pow2_division(struct sljit_compiler* compiler, uint32_t k)
{
	int shift = 0;
	int status = SLJIT_SUCCESS;

	while (k > 1) {
		k >>= 1;
		shift++;
	}

	BJ_ASSERT(k == 1 && shift < 32);

	if (shift != 0) {
		status = sljit_emit_op2(compiler,
		    SLJIT_LSHR|SLJIT_INT_OP,
		    BJ_AREG, 0,
		    BJ_AREG, 0,
		    SLJIT_IMM, shift);
	}

	return status;
}

#if !defined(BPFJIT_USE_UDIV)
static sljit_uw
divide(sljit_uw x, sljit_uw y)
{

	return (uint32_t)x / (uint32_t)y;
}
#endif

/*
 * Generate A = A / div.
 * divt,divw are either SLJIT_IMM,pc->k or BJ_XREG,0.
 */
static int
emit_division(struct sljit_compiler* compiler, int divt, sljit_sw divw)
{
	int status;

#if BJ_XREG == SLJIT_RETURN_REG   || \
    BJ_XREG == SLJIT_SCRATCH_REG1 || \
    BJ_XREG == SLJIT_SCRATCH_REG2 || \
    BJ_AREG == SLJIT_SCRATCH_REG2
#error "Not supported assignment of registers."
#endif

#if BJ_AREG != SLJIT_SCRATCH_REG1
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV,
	    SLJIT_SCRATCH_REG1, 0,
	    BJ_AREG, 0);
	if (status != SLJIT_SUCCESS)
		return status;
#endif

	status = sljit_emit_op1(compiler,
	    SLJIT_MOV,
	    SLJIT_SCRATCH_REG2, 0,
	    divt, divw);
	if (status != SLJIT_SUCCESS)
		return status;

#if defined(BPFJIT_USE_UDIV)
	status = sljit_emit_op0(compiler, SLJIT_UDIV|SLJIT_INT_OP);

#if BJ_AREG != SLJIT_SCRATCH_REG1
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV,
	    BJ_AREG, 0,
	    SLJIT_SCRATCH_REG1, 0);
	if (status != SLJIT_SUCCESS)
		return status;
#endif
#else
	status = sljit_emit_ijump(compiler,
	    SLJIT_CALL2,
	    SLJIT_IMM, SLJIT_FUNC_OFFSET(divide));

#if BJ_AREG != SLJIT_RETURN_REG
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV,
	    BJ_AREG, 0,
	    SLJIT_RETURN_REG, 0);
	if (status != SLJIT_SUCCESS)
		return status;
#endif
#endif

	return status;
}

/*
 * Return true if pc is a "read from packet" instruction.
 * If length is not NULL and return value is true, *length will
 * be set to a safe length required to read a packet.
 */
static bool
read_pkt_insn(struct bpf_insn *pc, uint32_t *length)
{
	bool rv;
	uint32_t width;

	switch (BPF_CLASS(pc->code)) {
	default:
		rv = false;
		break;

	case BPF_LD:
		rv = BPF_MODE(pc->code) == BPF_ABS ||
		     BPF_MODE(pc->code) == BPF_IND;
		if (rv)
			width = read_width(pc);
		break;

	case BPF_LDX:
		rv = pc->code == (BPF_LDX|BPF_B|BPF_MSH);
		width = 1;
		break;
	}

	if (rv && length != NULL) {
		*length = (pc->k > UINT32_MAX - width) ?
		    UINT32_MAX : pc->k + width;
	}

	return rv;
}

/*
 * Set bj_check_length for all "read from packet" instructions
 * in a linear block of instructions [from, to).
 */
static void
set_check_length(struct bpf_insn *insns, struct bpfjit_insn_data *insn_dat,
    size_t from, size_t to, uint32_t length)
{

	for (; from < to; from++) {
		if (read_pkt_insn(&insns[from], NULL)) {
			insn_dat[from].bj_aux.bj_rdata.bj_check_length = length;
			length = 0;
		}
	}
}

/*
 * Get safe length for a jump destication.
 */
static uint32_t
get_safe_length(struct bpfjit_insn_data *insn_dat)
{
	struct bpfjit_jump *jmp;
	uint32_t rv;

	rv = UINT32_MAX;
	SLIST_FOREACH(jmp, &insn_dat->bj_jumps, bj_entries) {
		if (jmp->bj_safe_length < rv)
			rv = jmp->bj_safe_length;
	}

	return rv;
}

/*
 * The function divides instructions into blocks. Destination of a jump
 * instruction starts a new block. BPF_RET and BPF_JMP instructions
 * terminate a block. Blocks are linear, that is, there are no jumps out
 * from the middle of a block and there are no jumps in to the middle of
 * a block.
 * If a block has one or more "read from packet" instructions,
 * bj_check_length will be set to one value for the whole block and that
 * value will be equal to the greatest value of safe lengths of "read from
 * packet" instructions inside the block.
 *
 * The function also sets bits in *initmask for memwords that
 * need to be initialized to zero. Note that this set should be empty
 * for any valid kernel filter program.
 */
static bool
optimize1(struct bpf_insn *insns,
    struct bpfjit_insn_data *insn_dat, size_t insn_count,
    bpfjit_init_mask_t *initmask, int *nscratches, int *ncopfuncs)
{
	struct bpfjit_jump *jtf;
	size_t i;
	size_t first_read;
	uint32_t jt, jf;
	uint32_t length, safe_length;
	bpfjit_init_mask_t invalid; /* borrowed from bpf_filter() */
	bool break_block, jump_dst, unreachable;

	*initmask = BJ_INIT_NOBITS;
	*nscratches = 2;
	*ncopfuncs = 0;

	for (i = 0; i < insn_count; i++) {
		insn_dat[i].bj_invalid = BJ_INIT_NOBITS;
		SLIST_INIT(&insn_dat[i].bj_jumps);
	}

	safe_length = 0;
	invalid = ~BJ_INIT_NOBITS;
	unreachable = false;
	first_read = SIZE_MAX;

	for (i = 0; i < insn_count; i++) {
		jump_dst = !SLIST_EMPTY(&insn_dat[i].bj_jumps);
		break_block =
		    insns[i].code == (BPF_MISC|BPF_COP) ||
		    insns[i].code == (BPF_MISC|BPF_COPX);

		if (jump_dst || (break_block && !unreachable)) {
			unreachable = false;

			set_check_length(insns, insn_dat,
			    first_read, i, safe_length);
			first_read = SIZE_MAX;

			if (jump_dst)
				safe_length = get_safe_length(&insn_dat[i]);
		}

		insn_dat[i].bj_unreachable = unreachable;
		if (unreachable)
			continue;

		invalid |= insn_dat[i].bj_invalid;

		if (read_pkt_insn(&insns[i], &length)) {
			if (first_read == SIZE_MAX)
				first_read = i;
			if (length > safe_length)
				safe_length = length;
		}

		switch (BPF_CLASS(insns[i].code)) {
		case BPF_RET:
			if (BPF_RVAL(insns[i].code) == BPF_A)
				*initmask |= invalid & BJ_INIT_ABIT;

			unreachable = true;
			continue;

		case BPF_LD:
			if (BPF_MODE(insns[i].code) == BPF_IND ||
			    BPF_MODE(insns[i].code) == BPF_ABS) {
				if (BPF_MODE(insns[i].code) == BPF_IND &&
				    *nscratches < 4) {
					/* uses BJ_XREG */
					*nscratches = 4;
				}
				if (*nscratches < 3 &&
				    read_width(&insns[i]) == 4) {
					/* uses BJ_TMP2REG */
					*nscratches = 3;
				}
			}

			if (BPF_MODE(insns[i].code) == BPF_IND)
				*initmask |= invalid & BJ_INIT_XBIT;

			if (BPF_MODE(insns[i].code) == BPF_MEM &&
			    (uint32_t)insns[i].k < BPF_MEMWORDS) {
				*initmask |= invalid & BJ_INIT_MBIT(insns[i].k);
			}

			invalid &= ~BJ_INIT_ABIT;
			continue;

		case BPF_LDX:
#if defined(_KERNEL)
			/* uses BJ_TMP3REG */
			*nscratches = 5;
#endif
			/* uses BJ_XREG */
			if (*nscratches < 4)
				*nscratches = 4;

			if (BPF_MODE(insns[i].code) == BPF_MEM &&
			    (uint32_t)insns[i].k < BPF_MEMWORDS) {
				*initmask |= invalid & BJ_INIT_MBIT(insns[i].k);
			}

			invalid &= ~BJ_INIT_XBIT;
			continue;

		case BPF_ST:
			*initmask |= invalid & BJ_INIT_ABIT;

			if ((uint32_t)insns[i].k < BPF_MEMWORDS)
				invalid &= ~BJ_INIT_MBIT(insns[i].k);

			continue;

		case BPF_STX:
			/* uses BJ_XREG */
			if (*nscratches < 4)
				*nscratches = 4;

			*initmask |= invalid & BJ_INIT_XBIT;

			if ((uint32_t)insns[i].k < BPF_MEMWORDS)
				invalid &= ~BJ_INIT_MBIT(insns[i].k);

			continue;

		case BPF_ALU:
			*initmask |= invalid & BJ_INIT_ABIT;

			if (insns[i].code != (BPF_ALU|BPF_NEG) &&
			    BPF_SRC(insns[i].code) == BPF_X) {
				*initmask |= invalid & BJ_INIT_XBIT;
				/* uses BJ_XREG */
				if (*nscratches < 4)
					*nscratches = 4;

			}

			invalid &= ~BJ_INIT_ABIT;
			continue;

		case BPF_MISC:
			switch (BPF_MISCOP(insns[i].code)) {
			case BPF_TAX: // X <- A
				/* uses BJ_XREG */
				if (*nscratches < 4)
					*nscratches = 4;

				*initmask |= invalid & BJ_INIT_ABIT;
				invalid &= ~BJ_INIT_XBIT;
				continue;

			case BPF_TXA: // A <- X
				/* uses BJ_XREG */
				if (*nscratches < 4)
					*nscratches = 4;

				*initmask |= invalid & BJ_INIT_XBIT;
				invalid &= ~BJ_INIT_ABIT;
				continue;

			case BPF_COPX:
				/* uses BJ_XREG */
				if (*nscratches < 4)
					*nscratches = 4;
				/* FALLTHROUGH */

			case BPF_COP:
				/* calls copfunc with three arguments */
				if (*nscratches < 3)
					*nscratches = 3;
			
				(*ncopfuncs)++;
				*initmask |= invalid & BJ_INIT_ABIT;
				invalid &= ~BJ_INIT_ABIT;
				// XXX Tweak MBITs.
				continue;
			}

			continue;

		case BPF_JMP:
			if (insns[i].code == (BPF_JMP|BPF_JA)) {
				jt = jf = insns[i].k;
			} else {
				jt = insns[i].jt;
				jf = insns[i].jf;
			}

			if (jt >= insn_count - (i + 1) ||
			    jf >= insn_count - (i + 1)) {
				return false;
			}

			if (jt > 0 && jf > 0)
				unreachable = true;

			jt += i + 1;
			jf += i + 1;

			jtf = insn_dat[i].bj_aux.bj_jdata.bj_jtf;

			jtf[0].bj_jump = NULL;
			jtf[0].bj_safe_length = safe_length;
			SLIST_INSERT_HEAD(&insn_dat[jt].bj_jumps,
			    &jtf[0], bj_entries);

			if (jf != jt) {
				jtf[1].bj_jump = NULL;
				jtf[1].bj_safe_length = safe_length;
				SLIST_INSERT_HEAD(&insn_dat[jf].bj_jumps,
				    &jtf[1], bj_entries);
			}

			insn_dat[jf].bj_invalid |= invalid;
			insn_dat[jt].bj_invalid |= invalid;
			invalid = 0;

			continue;
		}
	}

	set_check_length(insns, insn_dat, first_read, insn_count, safe_length);

	return true;
}

/*
 * Convert BPF_ALU operations except BPF_NEG and BPF_DIV to sljit operation.
 */
static int
bpf_alu_to_sljit_op(struct bpf_insn *pc)
{

	/*
	 * Note: all supported 64bit arches have 32bit multiply
	 * instruction so SLJIT_INT_OP doesn't have any overhead.
	 */
	switch (BPF_OP(pc->code)) {
	case BPF_ADD: return SLJIT_ADD;
	case BPF_SUB: return SLJIT_SUB;
	case BPF_MUL: return SLJIT_MUL|SLJIT_INT_OP;
	case BPF_OR:  return SLJIT_OR;
	case BPF_AND: return SLJIT_AND;
	case BPF_LSH: return SLJIT_SHL;
	case BPF_RSH: return SLJIT_LSHR|SLJIT_INT_OP;
	default:
		BJ_ASSERT(false);
		return 0;
	}
}

/*
 * Convert BPF_JMP operations except BPF_JA to sljit condition.
 */
static int
bpf_jmp_to_sljit_cond(struct bpf_insn *pc, bool negate)
{
	/*
	 * Note: all supported 64bit arches have 32bit comparison
	 * instructions so SLJIT_INT_OP doesn't have any overhead.
	 */
	int rv = SLJIT_INT_OP;

	switch (BPF_OP(pc->code)) {
	case BPF_JGT:
		rv |= negate ? SLJIT_C_LESS_EQUAL : SLJIT_C_GREATER;
		break;
	case BPF_JGE:
		rv |= negate ? SLJIT_C_LESS : SLJIT_C_GREATER_EQUAL;
		break;
	case BPF_JEQ:
		rv |= negate ? SLJIT_C_NOT_EQUAL : SLJIT_C_EQUAL;
		break;
	case BPF_JSET:
		rv |= negate ? SLJIT_C_EQUAL : SLJIT_C_NOT_EQUAL;
		break;
	default:
		BJ_ASSERT(false);
	}

	return rv;
}

/*
 * Convert BPF_K and BPF_X to sljit register.
 */
static int
kx_to_reg(struct bpf_insn *pc)
{

	switch (BPF_SRC(pc->code)) {
	case BPF_K: return SLJIT_IMM;
	case BPF_X: return BJ_XREG;
	default:
		BJ_ASSERT(false);
		return 0;
	}
}

static sljit_sw
kx_to_reg_arg(struct bpf_insn *pc)
{

	switch (BPF_SRC(pc->code)) {
	case BPF_K: return (uint32_t)pc->k; /* SLJIT_IMM, pc->k, */
	case BPF_X: return 0;               /* BJ_XREG, 0,      */
	default:
		BJ_ASSERT(false);
		return 0;
	}
}

bpfjit_function_t
bpfjit_generate_code(bpf_ctx_t *bc, struct bpf_insn *insns, size_t insn_count)
{
	void *rv;
	struct sljit_compiler *compiler;

	size_t i;
	int status;
	int branching, negate;
	unsigned int rval, mode, src;

	/* optimization related */
	bpfjit_init_mask_t initmask;
	int nscratches, ncopfuncs;

	/* a list of jumps to out-of-bound return from a generated function */
	struct sljit_jump **ret0;
	size_t ret0_size, ret0_maxsize;

	struct bpf_insn *pc;
	struct bpfjit_insn_data *insn_dat;

	/* for local use */
	struct sljit_label *label;
	struct sljit_jump *jump;
	struct bpfjit_jump *bjump, *jtf;

	struct sljit_jump *to_mchain_jump;

	uint32_t jt, jf;

	rv = NULL;
	compiler = NULL;
	insn_dat = NULL;
	ret0 = NULL;

	if (insn_count == 0 || insn_count > SIZE_MAX / sizeof(insn_dat[0]))
		goto fail;

	insn_dat = BJ_ALLOC(insn_count * sizeof(insn_dat[0]));
	if (insn_dat == NULL)
		goto fail;

	if (!optimize1(insns, insn_dat, insn_count,
	    &initmask, &nscratches, &ncopfuncs)) {
		goto fail;
	}

#if defined(_KERNEL)
	/* bpf_filter() checks initialization of memwords. */
	BJ_ASSERT((initmask & BJ_INIT_MMASK) == 0);
#endif

	ret0_size = 0;
	ret0_maxsize = 64;
	ret0 = BJ_ALLOC(ret0_maxsize * sizeof(ret0[0]));
	if (ret0 == NULL)
		goto fail;

	compiler = sljit_create_compiler();
	if (compiler == NULL)
		goto fail;

#if !defined(_KERNEL) && defined(SLJIT_VERBOSE) && SLJIT_VERBOSE
	sljit_compiler_verbose(compiler, stderr);
#endif

	status = sljit_emit_enter(compiler,
	    2, nscratches, 3, sizeof(struct bpfjit_stack));
	if (status != SLJIT_SUCCESS)
		goto fail;

	if (ncopfuncs > 0) {
		/* save ctx argument */
		status = sljit_emit_op1(compiler,
		    SLJIT_MOV_P,
		    SLJIT_MEM1(SLJIT_LOCALS_REG),
		    offsetof(struct bpfjit_stack, ctx),
		    BJ_CTX_ARG, 0);
		if (status != SLJIT_SUCCESS)
			goto fail;
	}

	status = load_buf_buflen(compiler);
	if (status != SLJIT_SUCCESS)
		goto fail;

	/* Copy args->buflen to the register. */
	status = sljit_emit_op1(compiler,
	    SLJIT_MOV,
	    BJ_BUFLEN, 0,
	    SLJIT_MEM1(BJ_ARGS),
	    offsetof(struct bpf_args, buflen));
	if (status != SLJIT_SUCCESS)
		goto fail;

	for (i = 0; i < BPF_MEMWORDS; i++) {
		if (initmask & BJ_INIT_MBIT(i)) {
			status = sljit_emit_op1(compiler,
			    SLJIT_MOV_UI,
			    SLJIT_MEM1(SLJIT_LOCALS_REG),
			    offsetof(struct bpf_state, mem) +
			        i * sizeof(uint32_t),
			    SLJIT_IMM, 0);
			if (status != SLJIT_SUCCESS)
				goto fail;
		}
	}

	if (initmask & BJ_INIT_ABIT) {
		/* A = 0; */
		status = sljit_emit_op1(compiler,
		    SLJIT_MOV,
		    BJ_AREG, 0,
		    SLJIT_IMM, 0);
		if (status != SLJIT_SUCCESS)
			goto fail;
	}

	if (initmask & BJ_INIT_XBIT) {
		/* X = 0; */
		status = sljit_emit_op1(compiler,
		    SLJIT_MOV,
		    BJ_XREG, 0,
		    SLJIT_IMM, 0);
		if (status != SLJIT_SUCCESS)
			goto fail;
	}

	for (i = 0; i < insn_count; i++) {
		if (insn_dat[i].bj_unreachable)
			continue;

		to_mchain_jump = NULL;

		/*
		 * Resolve jumps to the current insn.
		 */
		label = NULL;
		SLIST_FOREACH(bjump, &insn_dat[i].bj_jumps, bj_entries) {
			if (bjump->bj_jump != NULL) {
				if (label == NULL)
					label = sljit_emit_label(compiler);
				if (label == NULL)
					goto fail;
				sljit_set_label(bjump->bj_jump, label);
			}
		}

		if (read_pkt_insn(&insns[i], NULL) &&
		    insn_dat[i].bj_aux.bj_rdata.bj_check_length > 0) {
			/* if (buflen < bj_check_length) return 0; */
			jump = sljit_emit_cmp(compiler,
			    SLJIT_C_LESS,
			    BJ_BUFLEN, 0,
			    SLJIT_IMM,
			    insn_dat[i].bj_aux.bj_rdata.bj_check_length);
			if (jump == NULL)
		  		goto fail;
#ifdef _KERNEL
			to_mchain_jump = jump;
#else
			if (!append_jump(jump, &ret0,
			    &ret0_size, &ret0_maxsize))
				goto fail;
#endif
		}

		pc = &insns[i];
		switch (BPF_CLASS(pc->code)) {

		default:
			goto fail;

		case BPF_LD:
			/* BPF_LD+BPF_IMM          A <- k */
			if (pc->code == (BPF_LD|BPF_IMM)) {
				status = sljit_emit_op1(compiler,
				    SLJIT_MOV,
				    BJ_AREG, 0,
				    SLJIT_IMM, (uint32_t)pc->k);
				if (status != SLJIT_SUCCESS)
					goto fail;

				continue;
			}

			/* BPF_LD+BPF_MEM          A <- M[k] */
			if (pc->code == (BPF_LD|BPF_MEM)) {
				if (pc->k >= BPF_MEMWORDS)
					goto fail;
				status = sljit_emit_op1(compiler,
				    SLJIT_MOV_UI,
				    BJ_AREG, 0,
				    SLJIT_MEM1(SLJIT_LOCALS_REG),
				    offsetof(struct bpf_state, mem) +
				        pc->k * sizeof(uint32_t));
				if (status != SLJIT_SUCCESS)
					goto fail;

				continue;
			}

			/* BPF_LD+BPF_W+BPF_LEN    A <- len */
			if (pc->code == (BPF_LD|BPF_W|BPF_LEN)) {
				status = sljit_emit_op1(compiler,
				    SLJIT_MOV,
				    BJ_AREG, 0,
				    SLJIT_MEM1(BJ_ARGS),
				    offsetof(struct bpf_args, wirelen));
				if (status != SLJIT_SUCCESS)
					goto fail;

				continue;
			}

			mode = BPF_MODE(pc->code);
			if (mode != BPF_ABS && mode != BPF_IND)
				goto fail;

			status = emit_pkt_read(compiler, pc,
			    to_mchain_jump, &ret0, &ret0_size, &ret0_maxsize);
			if (status != SLJIT_SUCCESS)
				goto fail;

			continue;

		case BPF_LDX:
			mode = BPF_MODE(pc->code);

			/* BPF_LDX+BPF_W+BPF_IMM    X <- k */
			if (mode == BPF_IMM) {
				if (BPF_SIZE(pc->code) != BPF_W)
					goto fail;
				status = sljit_emit_op1(compiler,
				    SLJIT_MOV,
				    BJ_XREG, 0,
				    SLJIT_IMM, (uint32_t)pc->k);
				if (status != SLJIT_SUCCESS)
					goto fail;

				continue;
			}

			/* BPF_LDX+BPF_W+BPF_LEN    X <- len */
			if (mode == BPF_LEN) {
				if (BPF_SIZE(pc->code) != BPF_W)
					goto fail;
				status = sljit_emit_op1(compiler,
				    SLJIT_MOV,
				    BJ_XREG, 0,
				    SLJIT_MEM1(BJ_ARGS),
				    offsetof(struct bpf_args, wirelen));
				if (status != SLJIT_SUCCESS)
					goto fail;

				continue;
			}

			/* BPF_LDX+BPF_W+BPF_MEM    X <- M[k] */
			if (mode == BPF_MEM) {
				if (BPF_SIZE(pc->code) != BPF_W)
					goto fail;
				if (pc->k >= BPF_MEMWORDS)
					goto fail;
				status = sljit_emit_op1(compiler,
				    SLJIT_MOV_UI,
				    BJ_XREG, 0,
				    SLJIT_MEM1(SLJIT_LOCALS_REG),
				    offsetof(struct bpf_state, mem) +
				        pc->k * sizeof(uint32_t));
				if (status != SLJIT_SUCCESS)
					goto fail;

				continue;
			}

			/* BPF_LDX+BPF_B+BPF_MSH    X <- 4*(P[k:1]&0xf) */
			if (mode != BPF_MSH || BPF_SIZE(pc->code) != BPF_B)
				goto fail;

			status = emit_msh(compiler, pc,
			    to_mchain_jump, &ret0, &ret0_size, &ret0_maxsize);
			if (status != SLJIT_SUCCESS)
				goto fail;

			continue;

		case BPF_ST:
			if (pc->code != BPF_ST || pc->k >= BPF_MEMWORDS)
				goto fail;

			status = sljit_emit_op1(compiler,
			    SLJIT_MOV_UI,
			    SLJIT_MEM1(SLJIT_LOCALS_REG),
			    offsetof(struct bpf_state, mem) + 
			        pc->k * sizeof(uint32_t),
			    BJ_AREG, 0);
			if (status != SLJIT_SUCCESS)
				goto fail;

			continue;

		case BPF_STX:
			if (pc->code != BPF_STX || pc->k >= BPF_MEMWORDS)
				goto fail;

			status = sljit_emit_op1(compiler,
			    SLJIT_MOV_UI,
			    SLJIT_MEM1(SLJIT_LOCALS_REG),
			    offsetof(struct bpf_state, mem) + 
			        pc->k * sizeof(uint32_t),
			    BJ_XREG, 0);
			if (status != SLJIT_SUCCESS)
				goto fail;

			continue;

		case BPF_ALU:
			if (pc->code == (BPF_ALU|BPF_NEG)) {
				status = sljit_emit_op1(compiler,
				    SLJIT_NEG,
				    BJ_AREG, 0,
				    BJ_AREG, 0);
				if (status != SLJIT_SUCCESS)
					goto fail;

				continue;
			}

			if (BPF_OP(pc->code) != BPF_DIV) {
				status = sljit_emit_op2(compiler,
				    bpf_alu_to_sljit_op(pc),
				    BJ_AREG, 0,
				    BJ_AREG, 0,
				    kx_to_reg(pc), kx_to_reg_arg(pc));
				if (status != SLJIT_SUCCESS)
					goto fail;

				continue;
			}

			/* BPF_DIV */

			src = BPF_SRC(pc->code);
			if (src != BPF_X && src != BPF_K)
				goto fail;

			/* division by zero? */
			if (src == BPF_X) {
				jump = sljit_emit_cmp(compiler,
				    SLJIT_C_EQUAL|SLJIT_INT_OP,
				    BJ_XREG, 0, 
				    SLJIT_IMM, 0);
				if (jump == NULL)
					goto fail;
				if (!append_jump(jump, &ret0,
				    &ret0_size, &ret0_maxsize))
					goto fail;
			} else if (pc->k == 0) {
				jump = sljit_emit_jump(compiler, SLJIT_JUMP);
				if (jump == NULL)
					goto fail;
				if (!append_jump(jump, &ret0,
				    &ret0_size, &ret0_maxsize))
					goto fail;
			}

			if (src == BPF_X) {
				status = emit_division(compiler, BJ_XREG, 0);
				if (status != SLJIT_SUCCESS)
					goto fail;
			} else if (pc->k != 0) {
				if (pc->k & (pc->k - 1)) {
				    status = emit_division(compiler,
				        SLJIT_IMM, (uint32_t)pc->k);
				} else {
				    status = emit_pow2_division(compiler,
				        (uint32_t)pc->k);
				}
				if (status != SLJIT_SUCCESS)
					goto fail;
			}

			continue;

		case BPF_JMP:
			if (pc->code == (BPF_JMP|BPF_JA)) {
				jt = jf = pc->k;
			} else {
				jt = pc->jt;
				jf = pc->jf;
			}

			negate = (jt == 0) ? 1 : 0;
			branching = (jt == jf) ? 0 : 1;
			jtf = insn_dat[i].bj_aux.bj_jdata.bj_jtf;

			if (branching) {
				if (BPF_OP(pc->code) != BPF_JSET) {
					jump = sljit_emit_cmp(compiler,
					    bpf_jmp_to_sljit_cond(pc, negate),
					    BJ_AREG, 0,
					    kx_to_reg(pc), kx_to_reg_arg(pc));
				} else {
					status = sljit_emit_op2(compiler,
					    SLJIT_AND,
					    BJ_TMP1REG, 0,
					    BJ_AREG, 0,
					    kx_to_reg(pc), kx_to_reg_arg(pc));
					if (status != SLJIT_SUCCESS)
						goto fail;

					jump = sljit_emit_cmp(compiler,
					    bpf_jmp_to_sljit_cond(pc, negate),
					    BJ_TMP1REG, 0,
					    SLJIT_IMM, 0);
				}

				if (jump == NULL)
					goto fail;

				BJ_ASSERT(jtf[negate].bj_jump == NULL);
				jtf[negate].bj_jump = jump;
			}

			if (!branching || (jt != 0 && jf != 0)) {
				jump = sljit_emit_jump(compiler, SLJIT_JUMP);
				if (jump == NULL)
					goto fail;

				BJ_ASSERT(jtf[branching].bj_jump == NULL);
				jtf[branching].bj_jump = jump;
			}

			continue;

		case BPF_RET:
			rval = BPF_RVAL(pc->code);
			if (rval == BPF_X)
				goto fail;

			/* BPF_RET+BPF_K    accept k bytes */
			if (rval == BPF_K) {
				status = sljit_emit_return(compiler,
				    SLJIT_MOV_UI,
				    SLJIT_IMM, (uint32_t)pc->k);
				if (status != SLJIT_SUCCESS)
					goto fail;
			}

			/* BPF_RET+BPF_A    accept A bytes */
			if (rval == BPF_A) {
				status = sljit_emit_return(compiler,
				    SLJIT_MOV_UI,
				    BJ_AREG, 0);
				if (status != SLJIT_SUCCESS)
					goto fail;
			}

			continue;

		case BPF_MISC:
			switch (BPF_MISCOP(pc->code)) {
			case BPF_TAX:
				status = sljit_emit_op1(compiler,
				    SLJIT_MOV_UI,
				    BJ_XREG, 0,
				    BJ_AREG, 0);
				if (status != SLJIT_SUCCESS)
					goto fail;

				continue;

			case BPF_TXA:
				status = sljit_emit_op1(compiler,
				    SLJIT_MOV,
				    BJ_AREG, 0,
				    BJ_XREG, 0);
				if (status != SLJIT_SUCCESS)
					goto fail;

				continue;

			case BPF_COP:
			case BPF_COPX:
				jump = NULL;
				status = emit_cop(compiler, bc, pc, &jump);
				if (status != SLJIT_SUCCESS)
					goto fail;

				if (jump != NULL && !append_jump(jump,
				    &ret0, &ret0_size, &ret0_maxsize))
					goto fail;

				continue;
			}

			goto fail;
		} /* switch */
	} /* main loop */

	BJ_ASSERT(ret0_size <= ret0_maxsize);

	if (ret0_size > 0) {
		label = sljit_emit_label(compiler);
		if (label == NULL)
			goto fail;
		for (i = 0; i < ret0_size; i++)
			sljit_set_label(ret0[i], label);
	}

	status = sljit_emit_return(compiler,
	    SLJIT_MOV_UI,
	    SLJIT_IMM, 0);
	if (status != SLJIT_SUCCESS)
		goto fail;

	rv = sljit_generate_code(compiler);

fail:
	if (compiler != NULL)
		sljit_free_compiler(compiler);

	if (insn_dat != NULL)
		BJ_FREE(insn_dat, insn_count * sizeof(insn_dat[0]));

	if (ret0 != NULL)
		BJ_FREE(ret0, ret0_maxsize * sizeof(ret0[0]));

	return (bpfjit_function_t)rv;
}

void
bpfjit_free_code(bpfjit_function_t code)
{

	sljit_free_code((void *)code);
}
