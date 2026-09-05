#include "backend/llvm/native_abi.h"

#include "backend/llvm/emitter.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>

namespace {

DolLLVMFunctionRange *exactRange(std::vector<DolLLVMFunctionRange> &ranges,
                                 u32 start) {
  for (DolLLVMFunctionRange &range : ranges)
    if (range.start == start)
      return &range;
  return nullptr;
}

const DolLLVMFunctionRange *
addressRange(const std::vector<DolLLVMFunctionRange> &ranges, u32 address) {
  for (const DolLLVMFunctionRange &range : ranges)
    if (address >= range.start && address < range.end)
      return &range;
  return nullptr;
}

bool emptyABI(const DolLLVMFunctionRange &range) {
  if (range.abi_flags || range.abi_blockers || range.direct_memory_accesses ||
      range.generic_memory_accesses || range.helper_calls ||
      range.native_call_targets || range.native_call_depth)
    return false;
  for (u32 word = 0; word < DOLIR_STATE_MASK_WORDS; word++)
    if (range.semantic_input_state[word] || range.may_def_state[word] ||
        range.must_def_state[word] || range.callsite_live_after[word] ||
        range.semantic_output_state[word] || range.input_state[word] ||
        range.output_state[word] || range.escape_state[word])
      return false;
  return true;
}

} // namespace

namespace dolllvm {

bool needsInterpreter(const DolIRBlock &block) {
  for (u32 index = 0; index < block.instruction_count; index++) {
    const DolIRInstruction &instruction = block.instructions[index];
    if (instruction.op != DOLIR_OP_HELPER_CALL)
      continue;
    if (instruction.aux == DOLIR_HELPER_EXACT_FLOAT)
      return true;
    if (instruction.aux == DOLIR_HELPER_EXACT_PAIRED &&
        (instruction.immediate & 0xffu) > DOLIR_EXACT_PS_MULS1)
      return true;
  }
  return false;
}

void prepareModuleABIs(const DolIRModule &source,
                       std::vector<DolLLVMFunctionRange> &ranges,
                       DolLLVMRuntime runtime) {
  std::vector<DolLLVMCallEdge> edges;
  for (u32 index = 0; index < source.function_count; index++) {
    const DolIRFunction &function = source.functions[index];
    DolLLVMFunctionRange *range = exactRange(ranges, function.guest_start);
    if (!range)
      continue;
    if (emptyABI(*range))
      dolllvm_analyze_function_abi(&function, range);
    for (u32 blockIndex = 0; blockIndex < function.block_count; blockIndex++) {
      const DolIRTerminator &term = function.blocks[blockIndex].terminator;
      u64 liveAfter[DOLIR_STATE_MASK_WORDS]{};
      u64 definedBefore[DOLIR_STATE_MASK_WORDS]{};
      dolllvm_analyze_callsite_state(&function, blockIndex,
                                     range->may_def_state, liveAfter,
                                     definedBefore);
      u32 count = term.kind == DOLIR_TERM_COND_BRANCH ? 2u
                  : term.kind == DOLIR_TERM_FALLTHROUGH ||
                          term.kind == DOLIR_TERM_BRANCH
                      ? 1u
                  : term.kind == DOLIR_TERM_INDIRECT ? 2u
                                                     : 0u;
      for (u32 slot = 0; slot < count; slot++) {
        const DolLLVMFunctionRange *target =
            addressRange(ranges, term.target_addresses[slot]);
        if (target && target->start != range->start) {
          DolLLVMCallEdge edge{};
          edge.caller_start = range->start;
          edge.callee_address = term.target_addresses[slot];
          std::copy(liveAfter, liveAfter + DOLIR_STATE_MASK_WORDS,
                    edge.live_after);
          std::copy(definedBefore, definedBefore + DOLIR_STATE_MASK_WORDS,
                    edge.defined_before);
          edges.push_back(edge);
        }
      }
    }
  }
  if (runtime == DOLLLVM_RUNTIME_MODERNGEKKO)
    dolllvm_enable_native_services(ranges.data(),
                                   static_cast<u32>(ranges.size()));
  dolllvm_propagate_function_abis(ranges.data(),
                                  static_cast<u32>(ranges.size()), edges.data(),
                                  static_cast<u32>(edges.size()));
}

bool FunctionEmitter::stateInput(const DolLLVMFunctionRange *range,
                                 DolIRStateSlot slot) const {
  return range && (dolir_state_mask_test(range->input_state, slot) ||
                   dolir_state_mask_test(range->escape_state, slot));
}

bool FunctionEmitter::stateOutput(const DolLLVMFunctionRange *range,
                                  DolIRStateSlot slot) const {
  return range && dolir_state_mask_test(range->output_state, slot);
}

llvm::Type *
FunctionEmitter::nativeResultType(const DolLLVMFunctionRange *range) {
  llvm::SmallVector<llvm::Type *, 32> fields;
  if (!cold_escapes_) {
    fields.push_back(llvm::Type::getInt32Ty(context_));
    fields.push_back(llvm::Type::getInt1Ty(context_));
  }
  if (cold_escapes_) {
    const u32 lanes = nativeResultLaneCount(range);
    if (!lanes)
      return llvm::Type::getVoidTy(context_);
    if (lanes == 1)
      return llvm::Type::getInt64Ty(context_);
    fields.assign(lanes, llvm::Type::getInt64Ty(context_));
  } else {
    for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
      auto stateSlot = static_cast<DolIRStateSlot>(slot);
      if (stateOutput(range, stateSlot))
        fields.push_back(type(dolir_state_type(stateSlot)));
    }
  }
  return llvm::StructType::get(context_, fields);
}

llvm::StructType *FunctionEmitter::chainType() {
  llvm::Type *pointer = llvm::PointerType::getUnqual(context_);
  const u32 bufferWords = intrinsic_escapes_ ? 5u : 64u;
  return llvm::StructType::get(
      context_,
      {llvm::ArrayType::get(pointer, bufferWords),
       llvm::Type::getInt64Ty(context_), llvm::Type::getInt64Ty(context_),
       llvm::Type::getInt64Ty(context_), llvm::Type::getInt64Ty(context_),
       llvm::Type::getInt32Ty(context_), llvm::Type::getInt32Ty(context_),
       llvm::Type::getInt32Ty(context_),
       llvm::ArrayType::get(llvm::Type::getInt64Ty(context_),
                            DOLIR_STATE_COUNT)});
}

llvm::FunctionType *
FunctionEmitter::bodyFunctionType(const DolLLVMFunctionRange *range) {
  llvm::Type *pointer = llvm::PointerType::getUnqual(context_);
  llvm::SmallVector<llvm::Type *, 32> arguments = {pointer};
  if (modern_runtime_)
    arguments.push_back(pointer);
  arguments.push_back(pointer);
  arguments.push_back(llvm::Type::getInt64Ty(context_));
  const bool native =
      range && (range->abi_flags & DOLLLVM_FUNCTION_ABI_NATIVE) != 0;
  if (native) {
    if (nativeCyclesInResult(range))
      arguments.push_back(llvm::Type::getInt64Ty(context_));
    for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
      auto stateSlot = static_cast<DolIRStateSlot>(slot);
      if (stateInput(range, stateSlot))
        arguments.push_back(type(dolir_state_type(stateSlot)));
    }
  }
  llvm::Type *result = native
                           ? static_cast<llvm::Type *>(nativeResultType(range))
                           : llvm::Type::getVoidTy(context_);
  return llvm::FunctionType::get(result, arguments, false);
}

llvm::CallingConv::ID FunctionEmitter::bodyCallingConvention() const {
  return llvm::CallingConv::Fast;
}

} // namespace dolllvm
