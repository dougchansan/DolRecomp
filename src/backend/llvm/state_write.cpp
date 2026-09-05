#include "backend/llvm/emitter.h"
#include "backend/llvm/state_access.h"
#include "cpu/cpu.h"

#include <llvm/IR/Constants.h>

namespace dolllvm {

using namespace llvm;

void FunctionEmitter::storeContext(DolIRStateSlot slot, Value *value) {
  if (modern_runtime_) {
    Type *i32 = Type::getInt32Ty(context_);
    if (slot >= DOLIR_STATE_GPR0 && slot <= DOLIR_STATE_GPR31) {
      callStateWrite(WriteGPR,
                     {builder_.getInt32(slot - DOLIR_STATE_GPR0), value});
      return;
    }
    if (slot >= DOLIR_STATE_FPR0 && slot <= DOLIR_STATE_FPR31) {
      callStateWrite(
          WriteFPR,
          {builder_.getInt32(slot - DOLIR_STATE_FPR0), builder_.getInt32(0),
           builder_.CreateBitCast(value, Type::getInt64Ty(context_))});
      return;
    }
    if (slot >= DOLIR_STATE_PS1_0 && slot <= DOLIR_STATE_PS1_31) {
      callStateWrite(
          WriteFPR,
          {builder_.getInt32(slot - DOLIR_STATE_PS1_0), builder_.getInt32(1),
           builder_.CreateBitCast(value, Type::getInt64Ty(context_))});
      return;
    }
    if (slot >= DOLIR_STATE_CR0 && slot <= DOLIR_STATE_CR7) {
      Value *packed = callStateRead(ReadCR, {}, i32);
      const u32 shift = 28u - 4u * (slot - DOLIR_STATE_CR0);
      const u32 mask = 0xfu << shift;
      Value *field = builder_.CreateShl(
          builder_.CreateAnd(value, builder_.getInt32(0xf)), shift);
      callStateWrite(
          WriteCR,
          {builder_.CreateOr(
              builder_.CreateAnd(packed, builder_.getInt32(~mask)), field)});
      return;
    }
    if (slot >= DOLIR_STATE_XER_CA && slot <= DOLIR_STATE_XER_SO) {
      Value *packed = callStateRead(ReadXER, {}, i32);
      const u32 shift = 29u + (slot - DOLIR_STATE_XER_CA);
      const u32 mask = 1u << shift;
      Value *bit = builder_.CreateShl(builder_.CreateZExt(value, i32), shift);
      callStateWrite(
          WriteXER,
          {builder_.CreateOr(
              builder_.CreateAnd(packed, builder_.getInt32(~mask)), bit)});
      return;
    }
    if (slot >= DOLIR_STATE_SR0 && slot <= DOLIR_STATE_SR15) {
      callStateWrite(WriteSR,
                     {builder_.getInt32(slot - DOLIR_STATE_SR0), value});
      return;
    }
    if (slot >= DOLIR_STATE_GQR0 && slot <= DOLIR_STATE_GQR7) {
      callStateWrite(
          WriteSPR,
          {builder_.getInt32(SPR_GQR0 + slot - DOLIR_STATE_GQR0), value});
      return;
    }
    u32 spr = 0;
    switch (slot) {
    case DOLIR_STATE_LR:
      spr = SPR_LR;
      break;
    case DOLIR_STATE_CTR:
      spr = SPR_CTR;
      break;
    case DOLIR_STATE_SRR0:
      spr = SPR_SRR0;
      break;
    case DOLIR_STATE_SRR1:
      spr = SPR_SRR1;
      break;
    case DOLIR_STATE_DAR:
      spr = SPR_DAR;
      break;
    case DOLIR_STATE_DSISR:
      spr = SPR_DSISR;
      break;
    case DOLIR_STATE_EAR:
      spr = SPR_EAR;
      break;
    case DOLIR_STATE_HID2:
      spr = SPR_HID2;
      break;
    default:
      break;
    }
    if (spr) {
      callStateWrite(WriteSPR, {builder_.getInt32(spr), value});
      return;
    }
    switch (slot) {
    case DOLIR_STATE_CR:
      callStateWrite(WriteCR, {value});
      return;
    case DOLIR_STATE_XER: {
      Value *packed = callStateRead(ReadXER, {}, i32);
      Value *flags = builder_.CreateAnd(packed, builder_.getInt32(0xe0000000u));
      Value *misc = builder_.CreateAnd(value, builder_.getInt32(0x1fffffffu));
      callStateWrite(WriteXER, {builder_.CreateOr(flags, misc)});
      return;
    }
    case DOLIR_STATE_FPSCR:
      callStateWrite(WriteFPSCR, {value});
      return;
    case DOLIR_STATE_MSR:
      callStateWrite(WriteMSR, {value});
      return;
    case DOLIR_STATE_EXCEPTION:
      callStateWrite(WriteExceptions, {value});
      return;
    case DOLIR_STATE_RESERVE_ADDR:
      callStateWrite(WriteReserveAddress, {value});
      return;
    case DOLIR_STATE_RESERVE_VALID:
      callStateWrite(WriteReserveValid, {builder_.CreateZExt(value, i32)});
      return;
    default:
      return;
    }
  }
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

} // namespace dolllvm
