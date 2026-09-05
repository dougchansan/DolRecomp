#ifndef DOLRECOMP_LLVM_STATE_ACCESS_H
#define DOLRECOMP_LLVM_STATE_ACCESS_H

#include "common/types.h"

namespace dolllvm {

enum StateField : u32 {
  StateContext = 2,
  ReadGPR = 3,
  WriteGPR = 4,
  ReadFPR = 5,
  WriteFPR = 6,
  ReadCR = 7,
  WriteCR = 8,
  ReadXER = 9,
  WriteXER = 10,
  ReadSPR = 11,
  WriteSPR = 12,
  ReadSR = 13,
  WriteSR = 14,
  ReadFPSCR = 15,
  WriteFPSCR = 16,
  ReadMSR = 17,
  WriteMSR = 18,
  ReadExceptions = 19,
  WriteExceptions = 20,
  ReadReserveAddress = 21,
  WriteReserveAddress = 22,
  ReadReserveValid = 23,
  WriteReserveValid = 24,
};

inline constexpr u32 SPR_LR = 8;
inline constexpr u32 SPR_CTR = 9;
inline constexpr u32 SPR_DSISR = 18;
inline constexpr u32 SPR_DAR = 19;
inline constexpr u32 SPR_SRR0 = 26;
inline constexpr u32 SPR_SRR1 = 27;
inline constexpr u32 SPR_EAR = 282;
inline constexpr u32 SPR_GQR0 = 912;
inline constexpr u32 SPR_HID2 = 920;

} // namespace dolllvm

#endif
