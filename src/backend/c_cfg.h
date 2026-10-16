// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DOLRECOMP_C_CFG_H
#define DOLRECOMP_C_CFG_H

#include "../common/types.h"
#include "../frontend/decoder.h"

typedef struct {
    u32 count;
    u8* leaders;
    u32* block_cycles;
    u8* materialize_pc;
    u8* return_targets;
    u32* loop_ends;
} CFunctionCFG;

bool c_function_cfg_build(CFunctionCFG* cfg, const PPCInst* insts, u32 count,
                          u32 function_address);
void c_function_cfg_destroy(CFunctionCFG* cfg);

bool c_function_cfg_contains(const CFunctionCFG* cfg, u32 function_address,
                             u32 address);
bool c_function_cfg_can_loop_directly(const CFunctionCFG* cfg,
                                      const PPCInst* insts,
                                      u32 function_address,
                                      u32 branch_index);

// dcbst/dcbf/dcbi/icbi are normally emitted as an interpreter fallback, which
// ends the block and returns to the dispatcher. The SDK's DCFlushRange family
// applies them one 32-byte line at a time, so a single range flush costs
// thousands of dispatch round trips. With DOLRECOMP_INLINE_CACHE_OPS=1 they are
// emitted inline against ppc_cache_control instead, which reaches the same host
// hook without leaving the chunk. Opt-in, so the default output is unchanged.
bool dolrecomp_inline_cache_ops(void);

// DOLRECOMP_INLINE_FP selects inline fast paths for the hottest floating-point
// instructions: 1 keeps FPSCR bit-exact, 2 additionally skips the FPRF update,
// which is architecturally dead on GC6E01. Returns the name suffix the
// generated calls should carry, or NULL for the ordinary helpers.
int dolrecomp_inline_fp(void);
const char* dolrecomp_fp_suffix(void);

#endif
