#include "backend/llvm/emitter.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/MDBuilder.h>

namespace dolllvm {

using namespace llvm;

void FunctionEmitter::emitCacheControl(const DolIRInstruction &inst) {
  constexpr u32 ServiceContinue = 0;
  constexpr u32 ServiceStop = 4;
  constexpr u32 ExitFallback = 2;
  constexpr u32 ExitStop = 5;
  Type *i32 = Type::getInt32Ty(context_);
  Type *pointer = PointerType::getUnqual(context_);
  StructType *servicesType = StructType::get(
      context_, {i32, i32, pointer, pointer, pointer, pointer, pointer});
  Value *services = runtimeField(11);
  Value *function = builder_.CreateLoad(
      pointer, builder_.CreateStructGEP(servicesType, services, 6));
  Value *serviceContext = builder_.CreateLoad(
      pointer, builder_.CreateStructGEP(servicesType, services, 2));
  CallInst *status = builder_.CreateCall(
      FunctionType::get(i32, {pointer, i32, i32, i32, i32}, false), function,
      {serviceContext, builder_.getInt32(inst.guest_pc),
       builder_.getInt32(inst.immediate), operand(inst, 0),
       stateValue(DOLIR_STATE_MSR)});
  status->addFnAttr(Attribute::NoUnwind);
  BasicBlock *resume =
      BasicBlock::Create(context_, "cache_service_resume", function_);
  BasicBlock *failed =
      BasicBlock::Create(context_, "cache_service_exit", function_);
  builder_.CreateCondBr(
      builder_.CreateICmpEQ(status, builder_.getInt32(ServiceContinue)), resume,
      failed, MDBuilder(context_).createBranchWeights(2000, 1));

  builder_.SetInsertPoint(failed);
  materialize(inst.guest_pc);
  Value *reason = builder_.CreateSelect(
      builder_.CreateICmpEQ(status, builder_.getInt32(ServiceStop)),
      builder_.getInt32(ExitStop), builder_.getInt32(ExitFallback));
  builder_.CreateStore(reason,
                       builder_.CreateStructGEP(chainType(), chain_, 7));
  returnFromBody();

  builder_.SetInsertPoint(resume);
}

} // namespace dolllvm
