//===- X64RepeatedString.h - Restartable string iterations -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate REP and reuse the ordinary lifter for each committed element.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64REPEATEDSTRING_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64REPEATEDSTRING_H

#include "X64SegmentEncoding.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/analysis/arch/x86_64/X64PagedSource.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/LowUndefinedEffects.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::analysis::detail {

struct X64RepeatedString {
  unsigned AddressBytes = 8;
  bool Source = false, Destination = false;
  bool Compare = false, NotEqual = false;
  std::vector<LowOp> Operations;
  LowInstructionUndefinedEffects Undefined;
};

inline std::optional<X64RepeatedString>
x64RepeatedString(llvm::ArrayRef<uint8_t> Bytes, uint64_t Address) {
  if (Bytes.empty() || Bytes.size() > 15)
    return std::nullopt;
  std::vector<uint8_t> Element;
  uint8_t Repeat = 0;
  bool Rex = false;
  for (auto Byte : Bytes) {
    if (Byte == 0xf2 || Byte == 0xf3) {
      if (Repeat || Rex)
        return std::nullopt;
      Repeat = Byte;
    } else {
      Rex |= (Byte & 0xf0) == 0x40;
      Element.push_back(Byte);
    }
  }
  size_t P = 0;
  X64SegmentPrefixes Prefix;
  if (!Repeat || !x64SegmentPrefixes(Element, P, Prefix) ||
      P + 1 != Element.size())
    return std::nullopt;
  const uint8_t Opcode = Element[P] & 0xfe;
  const bool Compare = Opcode == 0xa6 || Opcode == 0xae;
  if ((!Compare && Opcode != 0xa4 && Opcode != 0xaa && Opcode != 0xac) ||
      (!Compare && Repeat != 0xf3))
    return std::nullopt;
  X64RepeatedString Result;
  Result.AddressBytes = Prefix.AddressBytes;
  Result.Source = Opcode != 0xaa && Opcode != 0xae;
  Result.Destination = Opcode != 0xac;
  Result.Compare = Compare;
  Result.NotEqual = Repeat == 0xf2;
  Decoder Decode;
  DecodedInsn Insn{};
  if (!Decode.init(Arch::X64) ||
      !Decode.decodeOneForLift(Element.data(), Element.size(), Address, Insn) ||
      Insn.Size != Element.size())
    return std::nullopt;
  Decode.liftToLow(Insn, Result.Operations, {}, {}, &Result.Undefined);
  // The explicit paged profile owns this authenticated long-mode form. Keep
  // the ordinary lifter's Missing sidecar: this does not extend its proof ABI.
  if (Result.Operations.empty() || !Result.Undefined.Effects.empty())
    return std::nullopt;
  return Result;
}

template <class Writer>
void emitX64RepeatedString(Writer &W, const X64RepeatedString &F, uint64_t PC,
                           uint64_t NextPC) {
  using Exit = X64ContinuationExitV1;
  const auto K = [](uint64_t V, unsigned Bytes = 8) {
    return NdVar::scalar(V, Bytes);
  };
  // Binding precedes even zero-count completion and all operand accesses.
  W.pagedAccess(K((F.Source ? kX64PagedRepeatSource : 0) |
                  (F.Destination ? kX64PagedRepeatDestination : 0) |
                  (F.Compare ? kX64PagedRepeatCompare : 0)),
                F.AddressBytes, X64PagedAccess::BeginRepeat, false);
  const auto Count = W.read(NdVar::reg(x86reg::RCX, F.AddressBytes));
  const auto Empty = W.temporary(1);
  W.emit(NdOp::INT_EQUAL, Empty, {Count, K(0, F.AddressBytes)});
  W.exitIf(Empty, [&] { W.leave(Exit::Continue, K(NextPC)); });
  for (const auto &Op : F.Operations)
    W.operation(Op);
  const auto Remaining = W.temporary(F.AddressBytes);
  W.emit(NdOp::INT_SUB, Remaining, {Count, K(1, F.AddressBytes)});
  W.write(NdVar::reg(x86reg::RCX, F.AddressBytes), Remaining);
  auto More = W.temporary(1);
  W.emit(NdOp::INT_NOTEQUAL, More, {Remaining, K(0, F.AddressBytes)});
  if (F.Compare) {
    const auto Condition = W.temporary(1), Continue = W.temporary(1);
    W.emit(NdOp::INT_EQUAL, Condition,
           {W.read(NdVar::reg(x86reg::ZF, 1)), K(!F.NotEqual, 1)});
    W.emit(NdOp::BOOL_AND, Continue, {More, Condition});
    More = Continue;
  }
  W.exitIf(More, [&] {
    W.leave(static_cast<Exit>(X64PagedExit::RepeatContinue), K(PC));
  });
  W.leave(Exit::Continue, K(NextPC));
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64REPEATEDSTRING_H
