//===- X86APXAtomic.h - Native APX atomic contracts -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared native API, encoding and assembly ownership for APX transactions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86APXATOMIC_H
#define NEVERD_BACKEND_LLVM_X86_X86APXATOMIC_H

#include "neverd/backend/llvm/X86/X86MemoryAddressAsm.h"
#include "neverd/backend/llvm/X86/X86NativeMemoryAddress.h"
#include "neverd/ir/intrinsics/X86/X86AtomicMemory.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace neverd {

inline constexpr std::array<Intrinsic, 5> X86APXAtomicInventory = {
    Intrinsic::ApxRaoAdd, Intrinsic::ApxRaoAnd, Intrinsic::ApxRaoOr,
    Intrinsic::ApxRaoXor, Intrinsic::ApxCmpccXadd};
inline constexpr std::array<Intrinsic, 5> X86LegacyAtomicInventory = {
    Intrinsic::X86LegacyRaoAdd, Intrinsic::X86LegacyRaoAnd,
    Intrinsic::X86LegacyRaoOr, Intrinsic::X86LegacyRaoXor,
    Intrinsic::X86LegacyCmpccXadd};
inline constexpr std::string_view X86APXAtomicAsmMarker =
    "# neverd.apx.atomic ";
inline constexpr unsigned X86APXConditionCount = 16;

struct X86APXAtomicPlan {
  Intrinsic Id;
  unsigned Bytes;
  unsigned AddressBits;
  unsigned SegmentPrefix;
  unsigned Condition;
  Arch TargetArch = Arch::X64;

  bool legacy() const { return isX86LegacyAtomicIntrinsic(Id); }
  bool compare() const { return isX86CmpccXaddIntrinsic(Id); }
  unsigned addressBytes() const { return TargetArch == Arch::X86 ? 4 : 8; }
  std::string encoding() const {
    return !legacy()                 ? "{evex} "
           : compare()               ? "{vex} "
           : TargetArch == Arch::X64 ? "{rex} "
                                     : "";
  }
  unsigned arguments() const { return compare() ? 3 : 2; }
  unsigned argumentBytes(unsigned I) const {
    return I ? Bytes : addressBytes();
  }
  std::string operation() const {
    switch (x86AtomicSemanticId(Id)) {
    case Intrinsic::ApxRaoAdd:
      return "aadd";
    case Intrinsic::ApxRaoAnd:
      return "aand";
    case Intrinsic::ApxRaoOr:
      return "aor";
    case Intrinsic::ApxRaoXor:
      return "axor";
    default:
      return "cmpccxadd";
    }
  }
  std::string name() const {
    return operation() + std::to_string(8 * Bytes) +
           (legacy() ? ".legacy" : "");
  }
  std::string llvmName() const {
    return "llvm.x86." + name() + ".address" +
           (TargetArch == Arch::X86 ? "32" : "");
  }
  std::string builtin() const {
    return "__builtin_ia32_" + operation() + std::to_string(8 * Bytes) +
           (legacy() ? "_legacy" : "") + "_address" +
           (TargetArch == Arch::X86 ? "32" : "");
  }
  std::string mnemonic() const {
    constexpr const char *Conditions[] = {"o",  "no", "b",  "ae", "e", "ne",
                                          "be", "a",  "s",  "ns", "p", "np",
                                          "l",  "ge", "le", "g"};
    // GNU as infers RAO width from its register and rejects l/q suffixes.
    return compare() ? "cmp" + std::string(Conditions[Condition]) + "xadd"
                     : operation();
  }
  bool stackReference() const { return x86NativeMemoryIsStack(SegmentPrefix); }
  std::string segment() const { return x86NativeMemorySegment(SegmentPrefix); }
  std::string assembly() const {
    const auto Prefix = segment().empty() ? "" : "%" + segment() + ":";
    const auto Address = AddressBits == 16   ? (compare() ? "${1:w}" : "${0:w}")
                         : AddressBits == 32 ? (compare() ? "${1:k}" : "${0:k}")
                                             : (compare() ? "$1" : "$0");
    const auto Setup = stackReference()
                           ? std::string("movq ${") + (compare() ? "1" : "0") +
                                 ":q}, %rdx\nsubq %rbp, %rdx\n"
                           : "";
    const auto Memory = stackReference() ? std::string("(%rbp,%rdx)")
                                         : "(" + std::string(Address) + ")";
    return std::string(X86APXAtomicAsmMarker) + name() + " " +
           std::to_string(AddressBits) + " " + std::to_string(SegmentPrefix) +
           " " + std::to_string(Condition) + "\n" + Setup + encoding() +
           mnemonic() + " " + (compare() ? "$3, $0, " : "$1, ") + Prefix +
           Memory;
  }
  std::string constraints(bool LegacyBX = false) const {
    const std::string Base =
        LegacyBX ? "{bx}" : x86MemoryAddressAsm(AddressBits).LLVMConstraint;
    const std::string Register = legacy() ? "R" : "r";
    const auto Clobbers = stackReference() ? ",~{rdx},~{flags},~{memory}"
                          : compare()      ? ",~{flags},~{memory}"
                                           : ",~{memory}";
    return (compare() ? "=" + Register + "," + Base + ",0," + Register
                      : Base + "," + Register) +
           Clobbers;
  }
};

inline std::optional<X86APXAtomicPlan>
x86APXAtomicPlan(Intrinsic Id, unsigned Bytes, unsigned AddressBits,
                 unsigned SegmentPrefix, unsigned Condition = 0,
                 Arch A = Arch::X64) {
  if (!isX86AtomicIntrinsic(Id) || (Bytes != 4 && Bytes != 8) ||
      (A != Arch::X64 && A != Arch::X86) ||
      (A == Arch::X64
           ? (AddressBits != 32 && AddressBits != 64)
           : (!isX86LegacyAtomicIntrinsic(Id) || isX86CmpccXaddIntrinsic(Id) ||
              Bytes != 4 || (AddressBits != 16 && AddressBits != 32))) ||
      Condition >= X86APXConditionCount ||
      (!isX86CmpccXaddIntrinsic(Id) && Condition))
    return std::nullopt;
  if (!x86NativeMemoryControlIsValid(SegmentPrefix) ||
      (x86NativeMemoryIsStack(SegmentPrefix) &&
       (A != Arch::X64 || AddressBits != 64)))
    return std::nullopt;
  return X86APXAtomicPlan{Id, Bytes, AddressBits, SegmentPrefix, Condition, A};
}

inline std::optional<unsigned> x86APXSegmentPrefix(NdMemoryAddressSpace Space) {
  switch (Space) {
  case NdMemoryAddressSpace::Default:
    return 0;
  case NdMemoryAddressSpace::X86FS:
    return 0x64;
  case NdMemoryAddressSpace::X86GS:
    return 0x65;
  }
  return std::nullopt;
}

inline std::optional<unsigned>
x86APXAtomicNativePrefix(const ApxAtomicIntrinsicShape &S) {
  const auto Prefix = x86APXSegmentPrefix(S.MemoryAddressSpace);
  if (!Prefix)
    return std::nullopt;
  return *Prefix | S.segmentPrefix() |
         (S.stackReference() ? X86NativeStackReference : 0);
}

inline std::optional<X86APXAtomicPlan> x86APXAtomicName(std::string_view Name) {
  for (const auto Id : X86APXAtomicInventory)
    for (unsigned Bytes : {4u, 8u}) {
      auto P = x86APXAtomicPlan(Id, Bytes, 64, 0);
      if (P->llvmName() == Name)
        return P;
    }
  for (const auto Id : X86LegacyAtomicInventory)
    for (Arch A : {Arch::X86, Arch::X64})
      for (unsigned Bytes : {4u, 8u}) {
        auto P = x86APXAtomicPlan(Id, Bytes, A == Arch::X86 ? 32 : 64, 0, 0, A);
        if (P && P->llvmName() == Name)
          return P;
      }
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86APXATOMIC_H
