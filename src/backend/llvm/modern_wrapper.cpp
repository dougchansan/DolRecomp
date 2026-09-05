#include "backend/llvm/emitter.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

namespace dolllvm {

using namespace llvm;

bool FunctionEmitter::emitModernWrapper(raw_ostream &diagnostics) {
  Type *pointer = PointerType::getUnqual(context_);
  Type *i32 = Type::getInt32Ty(context_);
  StructType *exitType =
      StructType::get(context_, {i32, i32, i32, i32, i32, i32, i32});
  FunctionType *type =
      FunctionType::get(exitType, {pointer, pointer, i32, i32}, false);
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
  wrapper->getArg(0)->setName("runtime");
  wrapper->getArg(1)->setName("state");
  wrapper->getArg(2)->setName("entry_pc");
  wrapper->getArg(3)->setName("cycle_budget");
  wrapper->getArg(0)->addAttr(Attribute::NonNull);
  wrapper->getArg(1)->addAttr(Attribute::NonNull);

  BasicBlock *entry = BasicBlock::Create(context_, "entry", wrapper);
  IRBuilderBase::InsertPoint saved = builder_.saveIP();
  Argument *savedContext = ctx_;
  Argument *savedStateInterface = state_interface_;
  Value *savedEntryPC = entry_pc_;
  builder_.SetInsertPoint(entry);
  ctx_ = wrapper->getArg(0);
  state_interface_ = wrapper->getArg(1);
  entry_pc_ = wrapper->getArg(2);

  StructType *chainTy = chainType();
  AllocaInst *chain = builder_.CreateAlloca(chainTy, nullptr, "chain");
  chain->setAlignment(Align(16));
  builder_.CreateStore(builder_.getInt64(0),
                       builder_.CreateStructGEP(chainTy, chain, 1));
  builder_.CreateStore(builder_.getInt64(0),
                       builder_.CreateStructGEP(chainTy, chain, 2));
  builder_.CreateStore(builder_.getInt64(0),
                       builder_.CreateStructGEP(chainTy, chain, 3));
  builder_.CreateStore(
      builder_.CreateZExt(wrapper->getArg(3), Type::getInt64Ty(context_)),
      builder_.CreateStructGEP(chainTy, chain, 4));
  builder_.CreateStore(wrapper->getArg(2),
                       builder_.CreateStructGEP(chainTy, chain, 5));
  builder_.CreateStore(
      builder_.CreateAdd(wrapper->getArg(2), builder_.getInt32(4)),
      builder_.CreateStructGEP(chainTy, chain, 6));
  builder_.CreateStore(builder_.getInt32(0),
                       builder_.CreateStructGEP(chainTy, chain, 7));

  Value *returnPC =
      builder_.CreateAnd(loadContext(DOLIR_STATE_LR), builder_.getInt32(~3u));
  BasicBlock *escaped = nullptr;
  const u32 bufferWords = intrinsic_escapes_ ? 5u : 64u;
  ArrayType *bufferTy = ArrayType::get(pointer, bufferWords);
  Value *buffer = builder_.CreateStructGEP(chainTy, chain, 0);
  Value *jumped = nullptr;
  if (intrinsic_escapes_) {
    Value *frame = builder_.CreateCall(
        Intrinsic::getDeclaration(&module_, Intrinsic::frameaddress, {pointer}),
        {builder_.getInt32(0)});
    builder_.CreateStore(
        frame,
        builder_.CreateInBoundsGEP(
            bufferTy, buffer, {builder_.getInt64(0), builder_.getInt64(0)}));
    Value *stack = builder_.CreateCall(
        Intrinsic::getDeclaration(&module_, Intrinsic::stacksave, {pointer}));
    builder_.CreateStore(
        stack,
        builder_.CreateInBoundsGEP(
            bufferTy, buffer, {builder_.getInt64(0), builder_.getInt64(2)}));
    jumped = builder_.CreateCall(
        Intrinsic::getDeclaration(&module_, Intrinsic::eh_sjlj_setjmp),
        {buffer});
  } else {
    auto setjmp = module_.getOrInsertFunction(
        "_setjmp",
        FunctionType::get(Type::getInt32Ty(context_), {pointer}, false));
    if (auto *setjmpFunction = dyn_cast<Function>(setjmp.getCallee())) {
      setjmpFunction->addFnAttr(Attribute::ReturnsTwice);
      setjmpFunction->addFnAttr(Attribute::NoUnwind);
    }
    jumped = builder_.CreateCall(setjmp, {buffer});
  }
  BasicBlock *invoke = BasicBlock::Create(context_, "invoke", wrapper);
  escaped = BasicBlock::Create(context_, "escaped", wrapper);
  builder_.CreateCondBr(builder_.CreateICmpEQ(jumped, builder_.getInt32(0)),
                        invoke, escaped);
  builder_.SetInsertPoint(invoke);

  Value *control = builder_.CreateOr(
      builder_.CreateZExt(wrapper->getArg(2), Type::getInt64Ty(context_)),
      builder_.CreateShl(
          builder_.CreateZExt(returnPC, Type::getInt64Ty(context_)),
          builder_.getInt64(32)));
  SmallVector<Value *, 32> arguments = {wrapper->getArg(0), wrapper->getArg(1),
                                        chain, control};
  if (nativeCyclesInResult(abi_range_))
    arguments.push_back(builder_.getInt64(0));
  for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
    auto stateSlot = static_cast<DolIRStateSlot>(slot);
    if (stateInput(abi_range_, stateSlot))
      arguments.push_back(loadContext(stateSlot));
  }
  CallInst *body = builder_.CreateCall(function_, arguments);
  body->setCallingConv(bodyCallingConvention());
  body->addFnAttr(Attribute::NoInline);

  u32 resultField = 2;
  for (u32 slot = 0; slot < DOLIR_STATE_COUNT; slot++) {
    auto stateSlot = static_cast<DolIRStateSlot>(slot);
    if (!stateOutput(abi_range_, stateSlot))
      continue;
    Value *value = cold_escapes_
                       ? nativeOutputValue(body, abi_range_, stateSlot)
                       : builder_.CreateExtractValue(body, resultField++);
    storeContext(stateSlot, value);
  }
  Value *normalCycles =
      nativeCyclesInResult(abi_range_)
          ? nativeCycleValue(body, abi_range_)
          : builder_.CreateLoad(Type::getInt64Ty(context_),
                                builder_.CreateStructGEP(chainTy, chain, 3));
  Value *normalExit = UndefValue::get(exitType);
  normalExit = builder_.CreateInsertValue(normalExit, builder_.getInt32(0), 0);
  normalExit = builder_.CreateInsertValue(normalExit, returnPC, 1);
  normalExit = builder_.CreateInsertValue(
      normalExit, builder_.CreateAdd(returnPC, builder_.getInt32(4)), 2);
  normalExit = builder_.CreateInsertValue(
      normalExit,
      builder_.CreateTrunc(normalCycles, Type::getInt32Ty(context_)), 3);
  normalExit = builder_.CreateInsertValue(normalExit, builder_.getInt32(0), 4);
  normalExit = builder_.CreateInsertValue(normalExit, builder_.getInt32(0), 5);
  normalExit = builder_.CreateInsertValue(normalExit, builder_.getInt32(0), 6);
  builder_.CreateRet(normalExit);

  builder_.SetInsertPoint(escaped);
  Value *escapeExit = UndefValue::get(exitType);
  escapeExit = builder_.CreateInsertValue(
      escapeExit,
      builder_.CreateLoad(i32, builder_.CreateStructGEP(chainTy, chain, 7)), 0);
  escapeExit = builder_.CreateInsertValue(
      escapeExit,
      builder_.CreateLoad(i32, builder_.CreateStructGEP(chainTy, chain, 5)), 1);
  escapeExit = builder_.CreateInsertValue(
      escapeExit,
      builder_.CreateLoad(i32, builder_.CreateStructGEP(chainTy, chain, 6)), 2);
  escapeExit = builder_.CreateInsertValue(
      escapeExit,
      builder_.CreateTrunc(
          builder_.CreateLoad(Type::getInt64Ty(context_),
                              builder_.CreateStructGEP(chainTy, chain, 3)),
          i32),
      3);
  escapeExit = builder_.CreateInsertValue(escapeExit, builder_.getInt32(0), 4);
  escapeExit = builder_.CreateInsertValue(escapeExit, builder_.getInt32(0), 5);
  escapeExit = builder_.CreateInsertValue(escapeExit, builder_.getInt32(0), 6);
  builder_.CreateRet(escapeExit);

  ctx_ = savedContext;
  state_interface_ = savedStateInterface;
  entry_pc_ = savedEntryPC;
  builder_.restoreIP(saved);
  return !verifyFunction(*wrapper, &diagnostics);
}


} // namespace dolllvm
