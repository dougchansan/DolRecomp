#include "backend/llvm/emitter.h"
#include "cpu/cpu.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>

namespace dolllvm {

using namespace llvm;

namespace {

constexpr u32 ServiceContinue = 0;
constexpr u32 ServiceException = 1;
constexpr u32 ServiceStop = 4;
constexpr u32 ExitException = 1;
constexpr u32 ExitFallback = 2;
constexpr u32 ExitStop = 5;

} // namespace

Value *FunctionEmitter::externalRead(Value *address, u32 width) {
  if (modern_runtime_) {
    Type *i32 = Type::getInt32Ty(context_);
    Type *i64 = Type::getInt64Ty(context_);
    Type *pointer = PointerType::getUnqual(context_);
    StructType *servicesType =
        StructType::get(context_, {i32, i32, pointer, pointer, pointer});
    StructType *resultType = StructType::get(context_, {i64, i32, i32});
    Value *services = runtimeField(11);
    Value *function = builder_.CreateLoad(
        pointer, builder_.CreateStructGEP(servicesType, services, 3));
    materialize(current_pc_);
    Value *serviceContext = builder_.CreateLoad(
        pointer, builder_.CreateStructGEP(servicesType, services, 2));
    CallInst *result = builder_.CreateCall(
        FunctionType::get(resultType, {pointer, i32, i32, i32}, false),
        function,
        {serviceContext, builder_.getInt32(current_pc_), address,
         builder_.getInt32(width)});
    result->addFnAttr(Attribute::NoUnwind);
    Value *status = builder_.CreateExtractValue(result, 1);
    BasicBlock *resume =
        BasicBlock::Create(context_, "read_service_resume", function_);
    BasicBlock *failed =
        BasicBlock::Create(context_, "read_service_exit", function_);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(status, builder_.getInt32(ServiceContinue)),
        resume, failed, MDBuilder(context_).createBranchWeights(2000, 1));

    builder_.SetInsertPoint(failed);
    Value *reason = builder_.CreateSelect(
        builder_.CreateICmpEQ(status, builder_.getInt32(ServiceException)),
        builder_.getInt32(ExitException),
        builder_.CreateSelect(
            builder_.CreateICmpEQ(status, builder_.getInt32(ServiceStop)),
            builder_.getInt32(ExitStop), builder_.getInt32(ExitFallback)));
    builder_.CreateStore(reason,
                         builder_.CreateStructGEP(chainType(), chain_, 7));
    returnFromBody();

    builder_.SetInsertPoint(resume);
    reloadCallCounters();
    return builder_.CreateExtractValue(result, 0);
  }

  Type *pointer = PointerType::getUnqual(context_);
  Value *function = loadOffset(pointer, offsetof(CPUState, external_read));
  BasicBlock *call = BasicBlock::Create(context_, "read_external", function_);
  BasicBlock *zero = BasicBlock::Create(context_, "read_unmapped", function_);
  BasicBlock *join = BasicBlock::Create(context_, "read_slow_join", function_);
  builder_.CreateCondBr(builder_.CreateIsNotNull(function), call, zero,
                        MDBuilder(context_).createBranchWeights(2000, 1));
  builder_.SetInsertPoint(call);
  materialize(current_pc_);
  auto *functionType = FunctionType::get(
      Type::getInt64Ty(context_),
      {pointer, Type::getInt32Ty(context_), Type::getInt8Ty(context_)}, false);
  Value *called = builder_.CreateCall(
      functionType, function, {ctx_, address, builder_.getInt8(width)});
  Value *exception =
      loadOffset(Type::getInt32Ty(context_), offsetof(CPUState, exception));
  BasicBlock *resume =
      BasicBlock::Create(context_, "read_slow_resume", function_);
  BasicBlock *failed =
      BasicBlock::Create(context_, "read_slow_exit", function_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(exception, builder_.getInt32(0)),
                        resume, failed);
  builder_.SetInsertPoint(failed);
  returnFromBody();
  builder_.SetInsertPoint(resume);
  reloadUsedState();
  builder_.CreateBr(join);
  BasicBlock *calledEnd = builder_.GetInsertBlock();
  builder_.SetInsertPoint(zero);
  Value *empty = builder_.getInt64(0);
  builder_.CreateBr(join);
  builder_.SetInsertPoint(join);
  PHINode *phi = builder_.CreatePHI(Type::getInt64Ty(context_), 2);
  phi->addIncoming(called, calledEnd);
  phi->addIncoming(empty, zero);
  return phi;
}

void FunctionEmitter::externalWrite(Value *address, Value *value, u32 width) {
  if (modern_runtime_) {
    Type *i32 = Type::getInt32Ty(context_);
    Type *i64 = Type::getInt64Ty(context_);
    Type *pointer = PointerType::getUnqual(context_);
    StructType *servicesType =
        StructType::get(context_, {i32, i32, pointer, pointer, pointer});
    Value *services = runtimeField(11);
    Value *function = builder_.CreateLoad(
        pointer, builder_.CreateStructGEP(servicesType, services, 4));
    materialize(current_pc_);
    Value *serviceContext = builder_.CreateLoad(
        pointer, builder_.CreateStructGEP(servicesType, services, 2));
    CallInst *status = builder_.CreateCall(
        FunctionType::get(i32, {pointer, i32, i32, i64, i32}, false), function,
        {serviceContext, builder_.getInt32(current_pc_), address,
         builder_.CreateZExtOrTrunc(value, i64), builder_.getInt32(width)});
    status->addFnAttr(Attribute::NoUnwind);
    BasicBlock *resume =
        BasicBlock::Create(context_, "write_service_resume", function_);
    BasicBlock *failed =
        BasicBlock::Create(context_, "write_service_exit", function_);
    builder_.CreateCondBr(
        builder_.CreateICmpEQ(status, builder_.getInt32(ServiceContinue)),
        resume, failed, MDBuilder(context_).createBranchWeights(2000, 1));

    builder_.SetInsertPoint(failed);
    Value *reason = builder_.CreateSelect(
        builder_.CreateICmpEQ(status, builder_.getInt32(ServiceException)),
        builder_.getInt32(ExitException),
        builder_.CreateSelect(
            builder_.CreateICmpEQ(status, builder_.getInt32(ServiceStop)),
            builder_.getInt32(ExitStop), builder_.getInt32(ExitFallback)));
    builder_.CreateStore(reason,
                         builder_.CreateStructGEP(chainType(), chain_, 7));
    returnFromBody();

    builder_.SetInsertPoint(resume);
    reloadCallCounters();
    return;
  }

  Type *pointer = PointerType::getUnqual(context_);
  Value *function = loadOffset(pointer, offsetof(CPUState, external_write));
  BasicBlock *call = BasicBlock::Create(context_, "write_external", function_);
  BasicBlock *done =
      BasicBlock::Create(context_, "write_slow_done", function_);
  builder_.CreateCondBr(builder_.CreateIsNotNull(function), call, done);
  builder_.SetInsertPoint(call);
  materialize(current_pc_);
  auto *functionType = FunctionType::get(
      Type::getVoidTy(context_),
      {pointer, Type::getInt32Ty(context_), Type::getInt64Ty(context_),
       Type::getInt8Ty(context_)},
      false);
  builder_.CreateCall(
      functionType, function,
      {ctx_, address,
       builder_.CreateZExtOrTrunc(value, Type::getInt64Ty(context_)),
       builder_.getInt8(width)});
  Value *exception =
      loadOffset(Type::getInt32Ty(context_), offsetof(CPUState, exception));
  BasicBlock *resume =
      BasicBlock::Create(context_, "write_slow_resume", function_);
  BasicBlock *failed =
      BasicBlock::Create(context_, "write_slow_exit", function_);
  builder_.CreateCondBr(builder_.CreateICmpEQ(exception, builder_.getInt32(0)),
                        resume, failed);
  builder_.SetInsertPoint(failed);
  returnFromBody();
  builder_.SetInsertPoint(resume);
  reloadUsedState();
  builder_.CreateBr(done);
  builder_.SetInsertPoint(done);
}

} // namespace dolllvm
