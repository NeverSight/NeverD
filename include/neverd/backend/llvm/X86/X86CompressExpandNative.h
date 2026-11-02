//===- X86CompressExpandNative.h - Compact memory transport ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native and owned assembly interfaces for one contiguous masked access.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_X86_X86COMPRESSEXPANDNATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X86COMPRESSEXPANDNATIVE_H
#include "neverd/backend/llvm/X86/X86NativeMemoryAddress.h"
#include "neverd/ir/intrinsics/X86/X86CompressExpand.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace neverd {
inline unsigned x86CompactNativePrefix(const X86CompactControl &C,
                                       NdMemoryAddressSpace Space) {
  return C.SegmentPrefix |
         (Space == NdMemoryAddressSpace::X86FS   ? 0x64
          : Space == NdMemoryAddressSpace::X86GS ? 0x65
                                                 : 0) |
         (C.StackReference ? X86NativeStackReference : 0);
}
inline constexpr std::string_view X86CompactAsmMarker = "# neverd.x86.compact ";
struct X86CompactPlan {
  bool Expand;
  unsigned Bytes;
  unsigned Kind;
  bool Zero;
  unsigned Prefix;
  unsigned elementBytes() const {
    constexpr unsigned Sizes[] = {1, 2, 4, 8, 4, 8};
    return Sizes[Kind];
  }
  unsigned maskBits() const { return std::max(16u, Bytes / elementBytes()); }
  std::string mnemonic() const {
    constexpr const char *Suffixes[] = {"b", "w", "d", "q", "ps", "pd"};
    return std::string(Kind < 4 ? "vp" : "v") +
           (Expand ? "expand" : "compress") + Suffixes[Kind];
  }
  std::string llvmName() const {
    return std::string("llvm.x86.avx512.") +
           (Expand ? "expandload" : "compressstore") +
           std::to_string(Bytes * 8) + ".native";
  }
  std::string builtin() const {
    return "__builtin_ia32_" + mnemonic() + std::to_string(Bytes * 8) +
           "_native";
  }
  std::vector<std::string> features() const {
    std::vector<std::string> R = {"avx512f"};
    if (Bytes < 64)
      R.push_back("avx512vl");
    else
      R.push_back("evex512");
    if (Kind < 2) {
      R.push_back("avx512bw");
      R.push_back("avx512vbmi2");
    }
    return R;
  }
  bool stackReference() const { return x86NativeMemoryIsStack(Prefix); }
  bool hasSegmentPrefix() const { return x86NativeMemoryPrefix(Prefix) != 0; }
  std::string segment() const { return x86NativeMemorySegment(Prefix); }
  std::string assembly() const {
    const auto Segment = hasSegmentPrefix() ? "%" + segment() + ":" : "";
    const auto Address = stackReference() ? "(%rbp,%rdx)"
                         : Expand         ? "($1)"
                                          : "($0)";
    const auto Setup = stackReference()
                           ? std::string("movq ${") + (Expand ? "1" : "0") +
                                 ":q}, %rdx\nsubq %rbp, %rdx\n"
                           : "";
    return std::string(X86CompactAsmMarker) + std::to_string(Expand) + " " +
           std::to_string(Bytes) + " " + std::to_string(Kind) + " " +
           std::to_string(Zero) + " " + std::to_string(Prefix) + "\n" + Setup +
           mnemonic() + " " +
           (Expand ? Segment + Address + ", $0 {$2}" + (Zero ? " {z}" : "")
                   : "$2, " + Segment + Address + " {$1}");
  }
  // AX cannot be a reserved frame, realignment or PIC base. Keep the old BX
  // spelling readable for owned IR emitted before this carrier correction.
  std::string constraints(bool LegacyBX = false) const {
    const std::string Base = LegacyBX ? "{bx}" : "{ax}";
    const auto Clobbers =
        stackReference() ? ",~{rdx},~{flags},~{memory}" : ",~{memory}";
    return (Expand ? "=v," + Base + ",^Yk,0" : Base + ",^Yk,v") + Clobbers;
  }
};
inline std::optional<X86CompactPlan> x86CompactPlan(bool Expand, unsigned Bytes,
                                                    unsigned Kind,
                                                    unsigned Zero,
                                                    unsigned Prefix) {
  if ((Bytes != 16 && Bytes != 32 && Bytes != 64) || Kind >= 6 || Zero > 1 ||
      (!Expand && Zero) || !x86NativeMemoryControlIsValid(Prefix))
    return {};
  return X86CompactPlan{Expand, Bytes, Kind, bool(Zero), Prefix};
}
inline std::optional<X86CompactPlan> x86CompactNativeName(std::string_view N) {
  for (bool Expand : {false, true})
    for (unsigned Bytes : {16u, 32u, 64u}) {
      X86CompactPlan P{Expand, Bytes, 0, false, 0};
      if (P.llvmName() == N)
        return P;
    }
  return {};
}
} // namespace neverd
#endif // NEVERD_BACKEND_LLVM_X86_X86COMPRESSEXPANDNATIVE_H
