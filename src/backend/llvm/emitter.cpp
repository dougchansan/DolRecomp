#include "backend/llvm/emitter.h"
#include "cpu/cpu.h"

#include <cstdio>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>

namespace dolllvm {

using namespace llvm;

FunctionEmitter::FunctionEmitter(LLVMContext &context, Module &module,
                                 const DolIRFunction &source,
                                 const DolLLVMOptions &options)
    : context_(context), module_(module), source_(source), builder_(context),
      ranges_(options.function_ranges),
      range_count_(options.function_range_count),
      entry_points_(options.entry_points),
      entry_point_count_(options.entry_point_count),
      write_journal_(options.instrumentation ==
                     DOLLLVM_INSTRUMENTATION_LOCKSTEP),
      semantics_(options.semantics),
      symbol_suffix_(options.symbol_suffix ? options.symbol_suffix : ""),
      fixed_memory_layout_(options.fixed_memory_layout != 0),
      state_in_memory_(options.state_in_memory != 0),
      expected_ram_size_(options.ram_size),
      expected_mem2_size_(options.mem2_size) {}

bool FunctionEmitter::emit(raw_ostream &diagnostics) {
  auto *pointer = PointerType::getUnqual(context_);
  auto *type = FunctionType::get(
      Type::getVoidTy(context_),
      {pointer, pointer, pointer, pointer, Type::getInt32Ty(context_)}, false);
  const std::string bodyName =
      symbolName(std::string(source_.name) + "_budget");
  function_ = module_.getFunction(bodyName);
  if (!function_)
    function_ =
        Function::Create(type, GlobalValue::ExternalLinkage, bodyName, module_);
  if (function_->getFunctionType() != type || !function_->empty()) {
    diagnostics << "dolllvm: conflicting native body " << bodyName << "\n";
    return false;
  }
  function_->setCallingConv(CallingConv::C);
  function_->setVisibility(GlobalValue::HiddenVisibility);
  function_->setDSOLocal(true);
  ctx_ = function_->getArg(0);
  ctx_->setName("ctx");
  ctx_->addAttr(Attribute::NonNull);
  ctx_->addAttr(Attribute::NoAlias);
  ctx_->addAttr(
      Attribute::getWithDereferenceableBytes(context_, sizeof(CPUState)));
  guard_cycles_ = function_->getArg(1);
  guard_cycles_->setName("guard_cycles");
  guard_steps_ = function_->getArg(2);
  guard_steps_->setName("guard_steps");
  pending_cycles_ = function_->getArg(3);
  pending_cycles_->setName("pending_cycles");
  entry_pc_ = function_->getArg(4);
  entry_pc_->setName("entry_pc");

  entry_ = BasicBlock::Create(context_, "entry", function_);
  fallback_block_ =
      BasicBlock::Create(context_, "fallback_dispatch", function_);
  IRBuilder<> fallbackBuilder(fallback_block_);
  fallback_pc_ =
      fallbackBuilder.CreatePHI(Type::getInt32Ty(context_), 0, "fallback_pc");
  scanState();
  scanStableControls();
  scanContinuations();
  scanLoopHeaders();
  scanRegionLeaders();
  blocks_.resize(source_.block_count);
  BasicBlock *region = nullptr;
  for (u32 i = 0; i < source_.block_count; i++) {
    if (source_.blocks[i].terminator.kind == DOLIR_TERM_FALLBACK) {
      blocks_[i] = fallback_block_;
      region = nullptr;
      continue;
    }
    if (region_leaders_[i])
      region = BasicBlock::Create(context_, blockName(i), function_);
    if (!region) {
      diagnostics << "dolllvm: instruction 0x"
                  << format_hex_no_prefix(source_.blocks[i].guest_address, 8)
                  << " has no native region\n";
      return false;
    }
    blocks_[i] = region;
  }
  emitEntry();
  for (u32 i = 0; i < source_.block_count; i++) {
    if (!region_leaders_[i] ||
        source_.blocks[i].terminator.kind == DOLIR_TERM_FALLBACK)
      continue;
    if (!emitRegion(i, diagnostics))
      return false;
  }
  emitFallbackHandler();
  finalizeStateSSA();
  if (verifyFunction(*function_, &diagnostics))
    return false;
  return emitWrapper(diagnostics);
}

bool FunctionEmitter::emitWrapper(raw_ostream &diagnostics) {
  auto *pointer = PointerType::getUnqual(context_);
  auto *type = FunctionType::get(Type::getVoidTy(context_), {pointer}, false);
  const std::string wrapperName = symbolName(source_.name);
  Function *wrapper = module_.getFunction(wrapperName);
  if (!wrapper)
    wrapper = Function::Create(type, GlobalValue::ExternalLinkage, wrapperName,
                               module_);
  if (wrapper->getFunctionType() != type || !wrapper->empty()) {
    diagnostics << "dolllvm: conflicting native entry " << source_.name << "\n";
    return false;
  }
  wrapper->setCallingConv(CallingConv::C);
  wrapper->setVisibility(GlobalValue::HiddenVisibility);
  wrapper->setDSOLocal(true);
  wrapper->getArg(0)->setName("ctx");
  wrapper->getArg(0)->addAttr(Attribute::NonNull);
  wrapper->getArg(0)->addAttr(
      Attribute::getWithDereferenceableBytes(context_, sizeof(CPUState)));

  BasicBlock *entry = BasicBlock::Create(context_, "entry", wrapper);
  IRBuilder<> builder(entry);
  AllocaInst *guardCycles =
      builder.CreateAlloca(Type::getInt64Ty(context_), nullptr, "guard_cycles");
  AllocaInst *guardSteps =
      builder.CreateAlloca(Type::getInt64Ty(context_), nullptr, "guard_steps");
  AllocaInst *pendingCycles = builder.CreateAlloca(Type::getInt64Ty(context_),
                                                   nullptr, "pending_cycles");
  builder.CreateStore(builder.getInt64(0), guardCycles);
  builder.CreateStore(builder.getInt64(0), guardSteps);
  builder.CreateStore(builder.getInt64(0), pendingCycles);
  Value *entryPC = builder.CreateLoad(
      Type::getInt32Ty(context_),
      builder.CreateInBoundsGEP(Type::getInt8Ty(context_), wrapper->getArg(0),
                                builder.getInt64(offsetof(CPUState, pc))));
  CallInst *body =
      builder.CreateCall(function_, {wrapper->getArg(0), guardCycles,
                                     guardSteps, pendingCycles, entryPC});
  body->addFnAttr(Attribute::NoInline);
  builder.CreateRetVoid();
  return !verifyFunction(*wrapper, &diagnostics);
}

std::string FunctionEmitter::blockName(u32 index) const {
  char text[40];
  snprintf(text, sizeof(text), "guest_%08X_b%u",
           source_.blocks[index].guest_address, index);
  return text;
}

std::string FunctionEmitter::symbolName(StringRef base) const {
  return base.str() + symbol_suffix_;
}

Type *FunctionEmitter::type(DolIRType t) {
  switch (t) {
  case DOLIR_TYPE_I1:
    return Type::getInt1Ty(context_);
  case DOLIR_TYPE_I8:
    return Type::getInt8Ty(context_);
  case DOLIR_TYPE_I16:
    return Type::getInt16Ty(context_);
  case DOLIR_TYPE_I32:
    return Type::getInt32Ty(context_);
  case DOLIR_TYPE_I64:
    return Type::getInt64Ty(context_);
  case DOLIR_TYPE_F32:
    return Type::getFloatTy(context_);
  case DOLIR_TYPE_F64:
    return Type::getDoubleTy(context_);
  case DOLIR_TYPE_V2F32:
    return FixedVectorType::get(Type::getFloatTy(context_), 2);
  case DOLIR_TYPE_V2F64:
    return FixedVectorType::get(Type::getDoubleTy(context_), 2);
  default:
    return Type::getVoidTy(context_);
  }
}

size_t FunctionEmitter::stateOffset(DolIRStateSlot slot) const {
  if (slot >= DOLIR_STATE_GPR0 && slot <= DOLIR_STATE_GPR31)
    return offsetof(CPUState, gpr) + 4u * (slot - DOLIR_STATE_GPR0);
  if (slot >= DOLIR_STATE_FPR0 && slot <= DOLIR_STATE_FPR31)
    return offsetof(CPUState, fpr) + 8u * (slot - DOLIR_STATE_FPR0);
  if (slot >= DOLIR_STATE_PS1_0 && slot <= DOLIR_STATE_PS1_31)
    return offsetof(CPUState, ps1) + 8u * (slot - DOLIR_STATE_PS1_0);
  if (slot >= DOLIR_STATE_SR0 && slot <= DOLIR_STATE_SR15)
    return offsetof(CPUState, sr) + 4u * (slot - DOLIR_STATE_SR0);
  if (slot >= DOLIR_STATE_GQR0 && slot <= DOLIR_STATE_GQR7)
    return offsetof(CPUState, gqr) + 4u * (slot - DOLIR_STATE_GQR0);
  switch (slot) {
  case DOLIR_STATE_PC:
    return offsetof(CPUState, pc);
  case DOLIR_STATE_LR:
    return offsetof(CPUState, lr);
  case DOLIR_STATE_CTR:
    return offsetof(CPUState, ctr);
  case DOLIR_STATE_CR:
    return offsetof(CPUState, cr);
  case DOLIR_STATE_XER:
    return offsetof(CPUState, xer);
  case DOLIR_STATE_FPSCR:
    return offsetof(CPUState, fpscr);
  case DOLIR_STATE_MSR:
    return offsetof(CPUState, msr);
  case DOLIR_STATE_SRR0:
    return offsetof(CPUState, srr0);
  case DOLIR_STATE_SRR1:
    return offsetof(CPUState, srr1);
  case DOLIR_STATE_DAR:
    return offsetof(CPUState, dar);
  case DOLIR_STATE_DSISR:
    return offsetof(CPUState, dsisr);
  case DOLIR_STATE_EAR:
    return offsetof(CPUState, ear);
  case DOLIR_STATE_HID2:
    return offsetof(CPUState, hid2);
  case DOLIR_STATE_TIMEBASE:
    return offsetof(CPUState, timebase);
  case DOLIR_STATE_EXCEPTION:
    return offsetof(CPUState, exception);
  case DOLIR_STATE_PROGRAM_EXCEPTION:
    return offsetof(CPUState, program_exception);
  case DOLIR_STATE_RESERVE_ADDR:
    return offsetof(CPUState, reserve_addr);
  case DOLIR_STATE_RESERVE_VALID:
    return offsetof(CPUState, reserve_valid);
  case DOLIR_STATE_DOWNCOUNT:
    return offsetof(CPUState, downcount);
  default:
    return 0;
  }
}

} // namespace dolllvm
