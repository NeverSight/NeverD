//===- AArch64SourceResultCall.h - Original x8 call transport --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A checked Darwin C call with an unproven result buffer must pass that exact
/// address in x8. A by-value result or LLVM sret would introduce storage and
/// validity promises. This bridge owns the complete call, including outgoing
/// stack arguments. x20 keeps the compiler's SP across the C ABI callee; the
/// reserved red zone protects caller locals even in generated leaf functions.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_AARCH64_AARCH64SOURCERESULTCALL_H
#define NEVERD_BACKEND_LLVM_AARCH64_AARCH64SOURCERESULTCALL_H

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <optional>
#include <string>

namespace neverd {
#define RESULT_CALL_VALUE(Name, Value) inline constexpr unsigned Name = Value;
#define RESULT_CALL_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AArch64SourceResultCall.def"
inline constexpr char kResultCallConstraints[] =
#define RESULT_CALL_INPUTS(Text) Text
#define RESULT_CALL_CLOBBER(Name) ",~{" Name "}"
#include "AArch64SourceResultCall.def"
    ;
inline constexpr char kResultCallCClobbers[] =
#define RESULT_CALL_CLOBBER(Name) "\"" Name "\", "
#include "AArch64SourceResultCall.def"
    ;

inline unsigned
sourceResultCallStackBytes(llvm::ArrayRef<SourceABIParameter> Bindings) {
  unsigned Bytes = 0;
  for (const auto &P : Bindings)
    if (P.Location.Kind == SourceABICarrierKind::Stack)
      Bytes = std::max(Bytes, unsigned(P.Location.EntryStackOffset) +
                                  P.Location.ValueBytes);
  return ((Bytes + kResultCallStackAlignment - 1) &
          -kResultCallStackAlignment) +
         kResultCallRedZoneBytes;
}

/// Bindings come from a signature that passed validateSourceABI. Each slot has
/// just its declared carrier; unused lanes and record padding are not read.
inline std::string
sourceResultCallAsm(llvm::ArrayRef<SourceABIParameter> Bindings,
                    std::optional<unsigned> Key) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  std::string Text = std::string(kResultCallMarker) + kResultCallPrefix;
  unsigned Slot = kResultCallHeaderWords;
  for (const auto &P : Bindings) {
    const auto &L = P.Location;
    const bool FP = L.Kind == SourceABICarrierKind::FloatingRegister;
    const bool Stack = L.Kind == SourceABICarrierKind::Stack;
    const auto &Bank = FP ? TRI.FPParamRegs : TRI.IntParamRegs;
    const unsigned Register =
        Stack ? kResultCallScratchRegister
              : std::find(Bank.begin(), Bank.end(), L.RegisterOffset) -
                    Bank.begin();
    const bool Signed = P.Type->IsSigned && L.ExtendTo32Bits;
    const char *Load =
        L.ValueBytes == kResultCallByteBytes
            ? (Signed ? kResultCallLoadSignedByte : kResultCallLoadByte)
        : L.ValueBytes == kResultCallHalfBytes
            ? (Signed ? kResultCallLoadSignedHalf : kResultCallLoadHalf)
            : kResultCallLoadWord;
    const char *Reg =
        FP ? (L.ValueBytes == kResultCallFloatBytes ? kResultCallS
                                                    : kResultCallD)
           : (L.ValueBytes <= kResultCallFloatBytes ? kResultCallW
                                                    : kResultCallX);
    Text += llvm::formatv(kResultCallLoad, Load, Reg, Register,
                          Slot++ * kResultCallWordBytes)
                .str();
    if (Stack)
      Text += llvm::formatv(
                  kResultCallStore,
                  L.ValueBytes == kResultCallByteBytes   ? kResultCallStoreByte
                  : L.ValueBytes == kResultCallHalfBytes ? kResultCallStoreHalf
                                                         : kResultCallStoreWord,
                  Reg, L.EntryStackOffset)
                  .str();
  }
  return Text + llvm::formatv(kResultCallSuffix, !Key ? kResultCallRaw
                                                 : *Key == kAArch64PAuthKeyIA
                                                     ? kResultCallIA
                                                     : kResultCallIB)
                    .str();
}

/// Render the same assembly program in GNU C. The second input requires a
/// compiler frame pointer even though the machine call does not consume it.
inline std::string sourceResultCallAsmC(llvm::StringRef Assembly,
                                        llvm::StringRef Packet,
                                        llvm::StringRef Frame) {
  std::string Text;
  for (size_t I = 0; I < Assembly.size(); ++I) {
    const char C = Assembly[I];
    if (C == '$')
      Text += '%';
    else if (C == '\n')
      Text += "\\n";
    else if (C == '"' || C == '\\') {
      Text += '\\';
      Text += C;
    } else
      Text += C;
  }
  return llvm::formatv(kResultCallAsmC, Text, Packet, Frame,
                       kResultCallCClobbers)
      .str();
}
} // namespace neverd
#endif // NEVERD_BACKEND_LLVM_AARCH64_AARCH64SOURCERESULTCALL_H
