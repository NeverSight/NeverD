//===- MIPSHistoricalTiming.h - Physical dependencies ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Non-interlocked dependencies of admitted MIPS I-III instructions. These
/// describe physical operands, including reads discarded by integer lowering.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_INTRINSICS_MIPS_MIPSHISTORICALTIMING_H
#define NEVERD_IR_INTRINSICS_MIPS_MIPSHISTORICALTIMING_H

#include "neverd/ir/intrinsics/MIPS/MIPSHistoricalISA.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalarFPOperations.h"

#include <cstdint>
#include <optional>

namespace neverd::mips {

enum HistoricalTimingAccess : uint16_t {
  ReadFCSR = 1u << 0,
  ReadFCC = 1u << 1,
  ReadHI = 1u << 2,
  ReadLO = 1u << 3,
  WriteHI = 1u << 4,
  WriteLO = 1u << 5,
  WriteFCSR = 1u << 6,
  WriteFCC = 1u << 7,
};

struct HistoricalInstructionTiming {
  uint64_t Reads = 0;
  uint64_t Delayed = 0;
  uint16_t Access = 0;
  uint32_t MergeSource = 0;
  uint32_t ForwardableLoad = 0;
};

inline std::optional<HistoricalInstructionTiming>
historicalInstructionTiming(uint32_t Word, unsigned Level, bool PairedFPRs) {
  if (Level < 1 || Level > 3 || !historicalInstructionAvailable(Word, Level))
    return std::nullopt;
  HistoricalInstructionTiming T;
  const unsigned Op = Word >> 26, RS = Word >> 21 & 31, RT = Word >> 16 & 31,
                 RD = Word >> 11 & 31, SA = Word >> 6 & 31, Fn = Word & 63;
  const auto GPR = [](unsigned R) { return R ? uint64_t{1} << R : 0; };
  const auto FPR = [PairedFPRs](unsigned R, unsigned Bytes) {
    return (uint64_t{1} << (32 + R)) |
           (PairedFPRs && Bytes == 8 ? uint64_t{1} << (33 + R) : 0);
  };
  if (Op == 0) {
    if (Fn == 16 || Fn == 18)
      T.Access = Fn == 16 ? ReadHI : ReadLO;
    else if (Fn == 17 || Fn == 19) {
      T.Reads = GPR(RS);
      T.Access = Fn == 17 ? WriteHI : WriteLO;
    } else if (Fn >= 24 && Fn <= 31) {
      T.Reads = GPR(RS) | GPR(RT);
      T.Access = WriteHI | WriteLO;
    } else if (Fn == 8 || Fn == 9)
      T.Reads = GPR(RS);
    else if (Fn == 0 || Fn == 2 || Fn == 3 || Fn >= 56)
      T.Reads = GPR(RT);
    else if (Fn != 12 && Fn != 13 && Fn != 15)
      T.Reads = GPR(RS) | GPR(RT);
    return T;
  }
  if (Op == 17) {
    if (RS < 8) {
      if ((RS == 1 || RS == 5) && PairedFPRs && (RD & 1))
        return std::nullopt;
      if (RS == 0 || RS == 1) {
        T.Reads = FPR(RD, RS == 1 ? 8 : 4);
        T.Delayed = GPR(RT);
      } else if (RS == 2) {
        T.Access = RD == 31 ? ReadFCSR : 0;
        T.Delayed = GPR(RT);
      } else if (RS == 4 || RS == 5) {
        T.Reads = GPR(RT);
        T.Delayed = FPR(RD, RS == 5 ? 8 : 4);
      } else if (RS == 6) {
        T.Reads = GPR(RT);
        T.Access = WriteFCSR;
      }
      return T;
    }
    if (RS == 8) {
      T.Access = ReadFCC;
      return T;
    }
    if ((RS == 16 || RS == 17) && Fn == 6 && !RT) {
      if (PairedFPRs && ((RD | SA) & 1))
        return std::nullopt;
      T.Reads = FPR(RD, RS == 17 ? 8 : 4);
      return T; // MOV.fmt does not observe or modify FCSR.
    }
    const auto Id = scalarFPArithmetic(Word, 0, PairedFPRs);
    if (!Id)
      return std::nullopt;
    const auto &F = scalarFPOperation(*Id);
    // Historical FR=0 arithmetic names even registers even for S/W operands;
    // word transfers and word loads/stores may still name an odd register.
    if (PairedFPRs && ((RD & 1) || (F.Arity >= 2 && (RT & 1)) ||
                       (!F.writesCondition() && (SA & 1))))
      return std::nullopt;
    T.Reads = FPR(RD, F.InputBytes);
    if (F.Arity >= 2)
      T.Reads |= FPR(RT, F.InputBytes);
    T.Access = ReadFCSR | (F.writesCondition() ? WriteFCC : 0);
    return T;
  }
  if (Op == 49 || Op == 53 || Op == 57 || Op == 61) {
    const unsigned Bytes = Op & 4 ? 8 : 4;
    if (PairedFPRs && Bytes == 8 && (RT & 1))
      return std::nullopt;
    T.Reads = GPR(RS);
    if (Op & 8)
      T.Reads |= FPR(RT, Bytes);
    else if (Level == 1)
      T.Delayed = FPR(RT, Bytes);
    return T;
  }
  if (Op == 2 || Op == 3 || Op == 15)
    return T;
  T.Reads = GPR(RS);
  if (Op == 4 || Op == 5 || Op == 20 || Op == 21 || (Op >= 40 && Op <= 46) ||
      Op == 56 || Op == 60 || Op == 63)
    T.Reads |= GPR(RT);
  const bool Merge = Op == 26 || Op == 27 || Op == 34 || Op == 38;
  if (Merge)
    T.MergeSource = GPR(RT);
  if (Level == 1 && Op >= 32 && Op <= 39) {
    T.Delayed = GPR(RT);
    T.ForwardableLoad = GPR(RT);
  }
  return T;
}

} // namespace neverd::mips

#endif // NEVERD_IR_INTRINSICS_MIPS_MIPSHISTORICALTIMING_H
