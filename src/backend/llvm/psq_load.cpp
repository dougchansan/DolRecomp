#include "backend/llvm/emitter.h"
#include "backend/llvm/psq_convert.h"
#include "cpu/cpu.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>

#include <utility>

namespace dolllvm {

using namespace llvm;

Value *FunctionEmitter::emitPSQLoad(const DolIRInstruction &inst,
                                    Value *address, Value *typeValue,
                                    Value *scaleValue) {
  u32 reg = inst.immediate & 0xFFu;
  bool w = ((inst.immediate >> 8) & 1u) != 0;
  u32 gqr = (inst.immediate >> 9) & 7u;
  bool indexed = ((inst.immediate >> 12) & 1u) != 0;
  auto incomingRepresentations = fp_rep_;
  auto incomingExactSingles = fp_exact_single_;
  auto incomingDenormalSafety = fp_denormal_safe_;
  auto incomingValueClasses = fp_value_class_;
  auto incomingKnownState = known_state_;
  bool incomingFPAvailable = fp_available_checked_;

  Value *enabled = psqEnabled(indexed);
  Value *supported = builder_.CreateOr(
      builder_.CreateOr(builder_.CreateICmpEQ(typeValue, builder_.getInt32(0)),
                        builder_.CreateICmpEQ(typeValue, builder_.getInt32(4))),
      builder_.CreateOr(
          builder_.CreateICmpEQ(typeValue, builder_.getInt32(5)),
          builder_.CreateOr(
              builder_.CreateICmpEQ(typeValue, builder_.getInt32(6)),
              builder_.CreateICmpEQ(typeValue, builder_.getInt32(7)))));
  Value *alignmentValid = builder_.CreateOr(
      builder_.CreateICmpNE(typeValue, builder_.getInt32(0)),
      builder_.CreateICmpEQ(builder_.CreateAnd(address, builder_.getInt32(3)),
                            builder_.getInt32(0)));

  BasicBlock *dispatch =
      BasicBlock::Create(context_, "psq_load_dispatch", function_);
  BasicBlock *slow = BasicBlock::Create(context_, "psq_load_helper", function_);
  BasicBlock *join = BasicBlock::Create(context_, "psq_load_join", function_);
  builder_.CreateCondBr(
      builder_.CreateAnd(builder_.CreateAnd(enabled, supported),
                         alignmentValid),
      dispatch, slow, MDBuilder(context_).createBranchWeights(1000, 1));

  BasicBlock *f32Block =
      BasicBlock::Create(context_, "psq_load_f32", function_);
  BasicBlock *u8Block = BasicBlock::Create(context_, "psq_load_u8", function_);
  BasicBlock *u16Block =
      BasicBlock::Create(context_, "psq_load_u16", function_);
  BasicBlock *s8Block = BasicBlock::Create(context_, "psq_load_s8", function_);
  BasicBlock *s16Block =
      BasicBlock::Create(context_, "psq_load_s16", function_);
  builder_.SetInsertPoint(dispatch);
  SwitchInst *typeSwitch = builder_.CreateSwitch(typeValue, slow, 5);
  typeSwitch->addCase(builder_.getInt32(0), f32Block);
  typeSwitch->addCase(builder_.getInt32(4), u8Block);
  typeSwitch->addCase(builder_.getInt32(5), u16Block);
  typeSwitch->addCase(builder_.getInt32(6), s8Block);
  typeSwitch->addCase(builder_.getInt32(7), s16Block);

  SmallVector<BasicBlock *, 5> fastEnds;
  auto emitLoads = [&](BasicBlock *block, u32 width, bool isSigned,
                       bool unquantized) {
    builder_.SetInsertPoint(block);
    auto loadLane = [&](Value *laneAddress) {
      if (unquantized) {
        Value *bits =
            emitGuestLoad(laneAddress, Type::getInt32Ty(context_), 4, false);
        return std::pair<Value *, Value *>(
            builder_.CreateBitCast(bits, Type::getFloatTy(context_)),
            extendPSQFloat(builder_, module_, bits));
      }
      Type *integerType = IntegerType::get(context_, width * 8);
      Value *integer = emitGuestLoad(laneAddress, integerType, width, false);
      Value *single =
          dequantizePSQInteger(builder_, integer, scaleValue, isSigned);
      return std::pair<Value *, Value *>(
          single, builder_.CreateFPExt(single, Type::getDoubleTy(context_)));
    };

    auto lane0 = loadLane(address);
    std::pair<Value *, Value *> lane1(
        ConstantFP::get(Type::getFloatTy(context_), 1.0),
        ConstantFP::get(Type::getDoubleTy(context_), 1.0));
    if (!w)
      lane1 = loadLane(builder_.CreateAdd(address, builder_.getInt32(width)));
    Type *pairType = FixedVectorType::get(Type::getFloatTy(context_), 2);
    Value *pair = PoisonValue::get(pairType);
    pair = builder_.CreateInsertElement(pair, lane0.first, uint64_t{0});
    pair = builder_.CreateInsertElement(pair, lane1.first, 1u);
    builder_.CreateStore(pair, pair_f32_[reg]);
    builder_.CreateStore(lane0.second, state_[DOLIR_STATE_FPR0 + reg]);
    builder_.CreateStore(lane1.second, state_[DOLIR_STATE_PS1_0 + reg]);
    builder_.CreateBr(join);
    fastEnds.push_back(builder_.GetInsertBlock());
  };
  emitLoads(f32Block, 4, false, true);
  emitLoads(u8Block, 1, false, false);
  emitLoads(u16Block, 2, false, false);
  emitLoads(s8Block, 1, true, false);
  emitLoads(s16Block, 2, true, false);

  builder_.SetInsertPoint(slow);
  Value *success = nullptr;
  BasicBlock *resume = nullptr;
  if (modern_runtime_) {
    sideExit(inst.guest_pc, 2);
  } else {
    Type *ptr = PointerType::getUnqual(context_);
    auto callee = module_.getOrInsertFunction(
        "ppc_psq_load",
        FunctionType::get(
            Type::getInt1Ty(context_),
            {ptr, Type::getInt8Ty(context_), Type::getInt32Ty(context_),
             Type::getInt1Ty(context_), Type::getInt8Ty(context_),
             Type::getInt1Ty(context_), Type::getInt32Ty(context_)},
            false));
    materialize(inst.guest_pc);
    success = builder_.CreateCall(
        callee, {ctx_, builder_.getInt8(reg), address, builder_.getInt1(w),
                 builder_.getInt8(gqr), builder_.getInt1(indexed),
                 builder_.getInt32(inst.guest_pc)});
    resume = BasicBlock::Create(context_, "psq_load_resume", function_);
    BasicBlock *failed =
        BasicBlock::Create(context_, "psq_load_exit", function_);
    builder_.CreateCondBr(success, resume, failed);
    builder_.SetInsertPoint(failed);
    returnFromBody();
    builder_.SetInsertPoint(resume);
    reloadUsedState();
    builder_.CreateStore(roundPairToSingle(pairF64(reg)), pair_f32_[reg]);
    builder_.CreateBr(join);
  }

  builder_.SetInsertPoint(join);
  Value *result = ConstantInt::getTrue(context_);
  if (!modern_runtime_) {
    PHINode *merged =
        builder_.CreatePHI(Type::getInt1Ty(context_), fastEnds.size() + 1);
    for (BasicBlock *fastEnd : fastEnds)
      merged->addIncoming(result, fastEnd);
    merged->addIncoming(success, resume);
    result = merged;
  }
  fp_rep_ = incomingRepresentations;
  fp_exact_single_ = incomingExactSingles;
  fp_denormal_safe_ = incomingDenormalSafety;
  fp_value_class_ = incomingValueClasses;
  known_state_ = incomingKnownState;
  fp_available_checked_ = incomingFPAvailable;
  fp_rep_[reg] = FPRepresentation::PairF32;
  fp_exact_single_[reg] = true;
  fp_denormal_safe_[reg] = false;
  fp_value_class_[reg] = FPValueClass::Unknown;
  psq_indexed_proven_ = true;
  if (!indexed)
    psq_direct_proven_ = true;
  return result;
}

} // namespace dolllvm
