#include "backend/llvm/emitter.h"
#include "cpu/cpu.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

namespace dolllvm {

using namespace llvm;

// Guest state can live in CPUState instead of being hoisted into allocas at
// region entry.
//
// Promoting the guest register file gives the register allocator far more
// simultaneously live values than x86-64 has registers, so it spills them
// straight back. Measured on GM4E01: 631 state phi nodes across 228 basic
// blocks in a single chunk, 20+ per block in hot loops. AArch64's 31 GPRs
// absorb much of that; x86-64's 16 do not.
//
// Note that skipping PromoteMemToReg alone achieves nothing: optimizeModule()
// runs the standard -O2 pipeline before emission and SROA/mem2reg promote the
// allocas anyway. The allocas have to not exist.
bool FunctionEmitter::stateInMemory() const { return state_in_memory_; }

// These slots are bitfields inside a wider CPUState word rather than
// addressable storage: CR0-CR7 are nibbles of cr, XER_CA-XER_SO are bits of
// xer, and XER itself preserves those flag bits while writing only the low 29.
// storeContext and loadContext pack and unpack them, so they cannot be pointed
// at directly and always keep an alloca.
bool FunctionEmitter::slotIsPacked(DolIRStateSlot slot) {
  return (slot >= DOLIR_STATE_CR0 && slot <= DOLIR_STATE_CR7) ||
         (slot >= DOLIR_STATE_XER_CA && slot <= DOLIR_STATE_XER_SO) ||
         slot == DOLIR_STATE_XER;
}

bool FunctionEmitter::slotInMemory(DolIRStateSlot slot) const {
  return stateInMemory() && !slotIsPacked(slot);
}

void FunctionEmitter::finalizeStateSSA() {
  // Nothing was hoisted, so there is nothing to promote.
  if (stateInMemory())
    return;
  SmallVector<AllocaInst *, DOLIR_STATE_COUNT + 64> registers;
  for (Value *slot : state_)
    if (auto *alloca = dyn_cast_or_null<AllocaInst>(slot))
      registers.push_back(alloca);
  for (AllocaInst *pair : pair_f32_)
    if (pair)
      registers.push_back(pair);
  for (AllocaInst *pair : pair_f64_)
    if (pair)
      registers.push_back(pair);
  if (registers.empty())
    return;
  DominatorTree dominators(*function_);
  PromoteMemToReg(registers, dominators);
}

void FunctionEmitter::resetFPRepresentations() {
  fp_rep_.fill(FPRepresentation::Raw);
  fp_exact_single_.fill(false);
  fp_denormal_safe_.fill(false);
  fp_value_class_.fill(FPValueClass::Unknown);
}

void FunctionEmitter::invalidateFPRepresentations() {
  resetFPRepresentations();
}

void FunctionEmitter::noteStateWrite(DolIRStateSlot slot, Value *value) {
  known_state_[slot] = dyn_cast<ConstantInt>(value);
  if (slot >= DOLIR_STATE_FPR0 && slot <= DOLIR_STATE_FPR31) {
    u32 reg = slot - DOLIR_STATE_FPR0;
    fp_rep_[reg] = FPRepresentation::Raw;
    fp_exact_single_[reg] = false;
    fp_denormal_safe_[reg] = false;
    fp_value_class_[reg] = FPValueClass::Unknown;
  } else if (slot >= DOLIR_STATE_PS1_0 && slot <= DOLIR_STATE_PS1_31) {
    u32 reg = slot - DOLIR_STATE_PS1_0;
    fp_rep_[reg] = FPRepresentation::Raw;
    fp_exact_single_[reg] = false;
    fp_denormal_safe_[reg] = false;
    fp_value_class_[reg] = FPValueClass::Unknown;
  } else if (slot == DOLIR_STATE_FPSCR) {
    pending_fprf_ = nullptr;
    fp_denormal_safe_.fill(false);
  } else if (slot == DOLIR_STATE_HID2) {
    psq_direct_proven_ = false;
    psq_indexed_proven_ = false;
  }
}

void FunctionEmitter::scanState() {
  for (u32 b = 0; b < source_.block_count; b++) {
    const DolIRBlock &block = source_.blocks[b];
    for (u32 i = 0; i < block.instruction_count; i++) {
      const DolIRInstruction &inst = block.instructions[i];
      for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
        auto stateSlot = static_cast<DolIRStateSlot>(slot);
        bool reads = dolir_state_mask_test(inst.state_uses, stateSlot);
        bool writes = dolir_state_mask_test(inst.state_defs, stateSlot);
        used_[slot] = used_[slot] || reads || writes;
        dirty_[slot] = dirty_[slot] || writes;
      }
    }
  }
}

void FunctionEmitter::scanContinuations() {
  for (u32 i = 0; i < source_.block_count; i++) {
    const DolIRTerminator &term = source_.blocks[i].terminator;
    if (!term.linked)
      continue;
    u32 continuation = term.guest_pc + 4u;
    u32 block = 0;
    if (continuation >= source_.guest_start &&
        continuation < source_.guest_end &&
        ((continuation - source_.guest_start) & 3u) == 0) {
      block = (continuation - source_.guest_start) / 4u;
      if (block < source_.block_count)
        continuations_.push_back(block);
    }
  }
}

void FunctionEmitter::scanLoopHeaders() {
  loop_headers_.assign(source_.block_count, false);
  for (u32 i = 0; i < source_.block_count; i++) {
    const DolIRTerminator &term = source_.blocks[i].terminator;
    u32 count = term.kind == DOLIR_TERM_COND_BRANCH ? 2u
                : term.kind == DOLIR_TERM_BRANCH    ? 1u
                                                    : 0u;
    for (u32 edge = 0; edge < count; edge++) {
      if (term.targets[edge] != DOLIR_NO_BLOCK && term.targets[edge] <= i)
        loop_headers_[term.targets[edge]] = true;
    }
  }
}

void FunctionEmitter::scanRegionLeaders() {
  region_leaders_.assign(source_.block_count, false);
  if (!source_.block_count)
    return;
  region_leaders_[0] = true;
  for (u32 i = 0; i < source_.block_count; i++) {
    const DolIRTerminator &term = source_.blocks[i].terminator;
    if (term.kind == DOLIR_TERM_FALLBACK)
      region_leaders_[i] = true;
    if (i + 1u < source_.block_count && term.kind != DOLIR_TERM_FALLTHROUGH)
      region_leaders_[i + 1u] = true;
    u32 count = term.kind == DOLIR_TERM_COND_BRANCH ? 2u
                : term.kind == DOLIR_TERM_BRANCH    ? 1u
                : term.kind == DOLIR_TERM_INDIRECT  ? 2u
                                                    : 0u;
    for (u32 edge = 0; edge < count; edge++) {
      if (term.targets[edge] != DOLIR_NO_BLOCK)
        region_leaders_[term.targets[edge]] = true;
    }
  }
  for (u32 i = 0; i < entry_point_count_; i++) {
    u32 address = entry_points_[i];
    if (address < source_.guest_start || address >= source_.guest_end ||
        ((address - source_.guest_start) & 3u) != 0)
      continue;
    region_leaders_[(address - source_.guest_start) / 4u] = true;
  }
}

Value *FunctionEmitter::bytePtr(size_t offset) {
  return builder_.CreateInBoundsGEP(Type::getInt8Ty(context_), ctx_,
                                    builder_.getInt64(offset));
}

Value *FunctionEmitter::loadContext(DolIRStateSlot slot) {
  if (slot >= DOLIR_STATE_CR0 && slot <= DOLIR_STATE_CR7) {
    Value *packed = builder_.CreateLoad(Type::getInt32Ty(context_),
                                        bytePtr(offsetof(CPUState, cr)));
    const u32 shift = 28u - 4u * (slot - DOLIR_STATE_CR0);
    return builder_.CreateAnd(builder_.CreateLShr(packed, shift),
                              builder_.getInt32(0xFu));
  }
  if (slot >= DOLIR_STATE_XER_CA && slot <= DOLIR_STATE_XER_SO) {
    Value *packed = builder_.CreateLoad(Type::getInt32Ty(context_),
                                        bytePtr(offsetof(CPUState, xer)));
    const u32 shift = 29u + (slot - DOLIR_STATE_XER_CA);
    return builder_.CreateTrunc(builder_.CreateLShr(packed, shift),
                                Type::getInt1Ty(context_));
  }
  return builder_.CreateLoad(type(dolir_state_type(slot)),
                             bytePtr(stateOffset(slot)));
}

void FunctionEmitter::storeContext(DolIRStateSlot slot, Value *value) {
  if (slot >= DOLIR_STATE_CR0 && slot <= DOLIR_STATE_CR7) {
    Value *pointer = bytePtr(offsetof(CPUState, cr));
    Value *packed = builder_.CreateLoad(Type::getInt32Ty(context_), pointer);
    const u32 shift = 28u - 4u * (slot - DOLIR_STATE_CR0);
    const u32 mask = 0xFu << shift;
    Value *kept = builder_.CreateAnd(packed, builder_.getInt32(~mask));
    Value *field = builder_.CreateShl(
        builder_.CreateAnd(value, builder_.getInt32(0xFu)), shift);
    builder_.CreateStore(builder_.CreateOr(kept, field), pointer);
    return;
  }
  if (slot >= DOLIR_STATE_XER_CA && slot <= DOLIR_STATE_XER_SO) {
    Value *pointer = bytePtr(offsetof(CPUState, xer));
    Value *packed = builder_.CreateLoad(Type::getInt32Ty(context_), pointer);
    const u32 shift = 29u + (slot - DOLIR_STATE_XER_CA);
    const u32 mask = 1u << shift;
    Value *kept = builder_.CreateAnd(packed, builder_.getInt32(~mask));
    Value *bit = builder_.CreateShl(
        builder_.CreateZExt(value, Type::getInt32Ty(context_)), shift);
    builder_.CreateStore(builder_.CreateOr(kept, bit), pointer);
    return;
  }
  if (slot == DOLIR_STATE_XER) {
    Value *pointer = bytePtr(offsetof(CPUState, xer));
    Value *packed = builder_.CreateLoad(Type::getInt32Ty(context_), pointer);
    Value *flags = builder_.CreateAnd(packed, builder_.getInt32(0xE0000000u));
    Value *misc = builder_.CreateAnd(value, builder_.getInt32(0x1FFFFFFFu));
    builder_.CreateStore(builder_.CreateOr(flags, misc), pointer);
    return;
  }
  builder_.CreateStore(value, bytePtr(stateOffset(slot)));
}

Value *FunctionEmitter::loadOffset(Type *valueType, size_t offset) {
  return builder_.CreateLoad(valueType, bytePtr(offset));
}

} // namespace dolllvm
