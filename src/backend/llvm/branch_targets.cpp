#include "backend/llvm/emitter.h"
#include "cpu/cpu.h"

#include <cstdio>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>

namespace dolllvm {

using namespace llvm;

BasicBlock *FunctionEmitter::directDestination(const DolIRTerminator &term,
                                               u32 slot) {
  if (term.targets[slot] != DOLIR_NO_BLOCK) {
    if (source_.blocks[term.targets[slot]].terminator.kind ==
        DOLIR_TERM_FALLBACK)
      return fallbackEdge(term.target_addresses[slot]);
    return blocks_[term.targets[slot]];
  }
  return externalDestination(term, slot);
}

const DolLLVMFunctionRange *FunctionEmitter::rangeFor(u32 address) const {
  for (u32 i = 0; i < range_count_; i++)
    if (address >= ranges_[i].start && address < ranges_[i].end)
      return &ranges_[i];
  return nullptr;
}

BasicBlock *FunctionEmitter::externalDestination(const DolIRTerminator &term,
                                                 u32 slot) {
  u32 target = term.target_addresses[slot];
  const DolLLVMFunctionRange *range = rangeFor(target);
  if (!range)
    return nullptr;
  BasicBlock *callBlock = BasicBlock::Create(
      context_, term.linked ? "direct_call" : "direct_tail", function_);
  IRBuilderBase::InsertPoint saved = builder_.saveIP();
  builder_.SetInsertPoint(callBlock);
  emitBudgetGuard(target);
  syncDirtyState();
  flushCallCounters();
  char name[64];
  snprintf(name, sizeof(name), "func_%08X_budget", range->start);
  const std::string targetName = symbolName(name);
  auto callee = module_.getOrInsertFunction(
      targetName, FunctionType::get(Type::getVoidTy(context_),
                              {PointerType::getUnqual(context_),
                               PointerType::getUnqual(context_),
                               PointerType::getUnqual(context_),
                               PointerType::getUnqual(context_),
                               Type::getInt32Ty(context_)}, false));
  if (auto *calleeFunction = dyn_cast<Function>(callee.getCallee())) {
    calleeFunction->setVisibility(GlobalValue::HiddenVisibility);
    calleeFunction->setDSOLocal(true);
  }
  CallInst *nativeCall = builder_.CreateCall(
      callee, {ctx_, guard_cycles_, guard_steps_, pending_cycles_,
               builder_.getInt32(target)});
  if (!term.linked) {
    nativeCall->setTailCallKind(CallInst::TCK_MustTail);
    builder_.CreateRetVoid();
    builder_.restoreIP(saved);
    return callBlock;
  }
  u32 continuation = term.guest_pc + 4u;
  u32 continuationBlock = 0;
  bool local = continuation >= source_.guest_start &&
               continuation < source_.guest_end &&
               ((continuation - source_.guest_start) & 3u) == 0;
  if (local)
    continuationBlock = (continuation - source_.guest_start) / 4u;
  BasicBlock *resume = BasicBlock::Create(context_, "call_resume", function_);
  BasicBlock *mismatch =
      BasicBlock::Create(context_, "call_mismatch", function_);
  Value *returnedPC =
      loadOffset(Type::getInt32Ty(context_), offsetof(CPUState, pc));
  builder_.CreateCondBr(
      builder_.CreateICmpEQ(returnedPC, builder_.getInt32(continuation)),
      resume, mismatch);
  builder_.SetInsertPoint(mismatch);
  builder_.CreateRetVoid();
  builder_.SetInsertPoint(resume);
  if (!local || continuationBlock >= blocks_.size()) {
    builder_.CreateRetVoid();
  } else {
    reloadCallCounters();
    for (u32 state = 0; state < DOLIR_STATE_COUNT; state++) {
      if (!used_[state])
        continue;
      auto stateSlot = static_cast<DolIRStateSlot>(state);
      if (slotInMemory(stateSlot))
        continue;
      builder_.CreateStore(loadContext(stateSlot), state_[state]);
    }
    builder_.CreateBr(blocks_[continuationBlock]);
  }
  builder_.restoreIP(saved);
  return callBlock;
}

BasicBlock *FunctionEmitter::exitDestination(u32 pc) {
  BasicBlock *exit = BasicBlock::Create(context_, "side_exit", function_);
  IRBuilderBase::InsertPoint saved = builder_.saveIP();
  builder_.SetInsertPoint(exit);
  sideExit(pc);
  builder_.restoreIP(saved);
  return exit;
}

BasicBlock *
FunctionEmitter::fallbackDestination(const DolIRTerminator &terminator) {
  BasicBlock *source = builder_.GetInsertBlock();
  fallback_pc_->addIncoming(builder_.getInt32(terminator.guest_pc), source);
  return fallback_block_;
}

BasicBlock *FunctionEmitter::fallbackEdge(u32 pc) {
  BasicBlock *edge = BasicBlock::Create(context_, "fallback_edge", function_);
  IRBuilderBase::InsertPoint saved = builder_.saveIP();
  builder_.SetInsertPoint(edge);
  fallback_pc_->addIncoming(builder_.getInt32(pc), edge);
  builder_.CreateBr(fallback_block_);
  builder_.restoreIP(saved);
  return edge;
}


} // namespace dolllvm
