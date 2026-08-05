//===- Intrinsics.cpp - Intrinsic function definitions -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Intrinsic operation metadata and classification.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdio>
#include <cstring>

namespace neverd {

namespace {

constexpr size_t kCount = static_cast<size_t>(Intrinsic::_Count);

} // anonymous namespace

const char *intrinsicName(Intrinsic Id) {
  if (Id == Intrinsic::None)
    return "none";
  if (Id == Intrinsic::Syscall)
    return "syscall";
  if (auto *CName = intrinsicCName(Id))
    return CName;
  if (auto *Asm = intrinsicAsmMnemonic(Id))
    return Asm;
  static thread_local char Buf[32];
  std::snprintf(Buf, sizeof(Buf), "<unknown_%u>", static_cast<unsigned>(Id));
  return Buf;
}

namespace {

struct CNameTable {
  const char *names[kCount] = {};

  constexpr CNameTable() {
    auto C = [this](Intrinsic Id, const char *N) {
      names[static_cast<size_t>(Id)] = N;
    };

#include "neverd/ir/intrinsics/intrinsics_aarch64_cnames.inc"
#include "neverd/ir/intrinsics/intrinsics_arm_cnames.inc"
#include "neverd/ir/intrinsics/intrinsics_x86_cnames.inc"
  }
};

constexpr CNameTable kCNames;

} // anonymous namespace

namespace {

struct AsmTable {
  const char *names[kCount] = {};

  constexpr AsmTable() {
    auto A = [this](Intrinsic Id, const char *N) {
      names[static_cast<size_t>(Id)] = N;
    };

#include "neverd/ir/intrinsics/intrinsics_aarch64_asm.inc"
#include "neverd/ir/intrinsics/intrinsics_arm_asm.inc"
#include "neverd/ir/intrinsics/intrinsics_x86_asm.inc"
  }
};

constexpr AsmTable kAsm;

} // anonymous namespace

const char *intrinsicAsmMnemonic(Intrinsic Id) {
  auto Idx = static_cast<size_t>(Id);
  if (Idx >= kCount)
    return nullptr;
  return kAsm.names[Idx];
}

const char *intrinsicCName(Intrinsic Id) {
  auto Idx = static_cast<size_t>(Id);
  if (Idx >= kCount)
    return nullptr;
  return kCNames.names[Idx];
}

namespace {

struct LlvmCNameEntry {
  const char *LlvmName;
  const char *CName;
};
constexpr LlvmCNameEntry kLlvmCNames[] = {
#include "neverd/ir/intrinsics/intrinsics_llvm_cnames.inc"

};

} // anonymous namespace

const char *llvmIntrinsicToCName(const char *LLVMName) {
  if (!LLVMName)
    return nullptr;
  for (auto &E : kLlvmCNames)
    if (std::strcmp(E.LlvmName, LLVMName) == 0)
      return E.CName;
  return nullptr;
}

namespace {

// Table-driven side-effect classification.  The per-architecture entries live
// in intrinsics_{x86,aarch64,arm}_sideeffect.inc; Syscall is architecture-
// generic.  Mirrors the CNameTable / AsmTable / MultiOutTable pattern above.
struct SideEffectTable {
  bool Flags[kCount] = {};

  constexpr SideEffectTable() {
    auto SIDEEFFECT = [this](Intrinsic Id) {
      Flags[static_cast<size_t>(Id)] = true;
    };

    SIDEEFFECT(Intrinsic::Syscall);
#include "neverd/ir/intrinsics/intrinsics_aarch64_sideeffect.inc"
#include "neverd/ir/intrinsics/intrinsics_arm_sideeffect.inc"
#include "neverd/ir/intrinsics/intrinsics_x86_sideeffect.inc"
  }
};

constexpr SideEffectTable kSideEffect;

} // anonymous namespace

bool isSideeffectIntrinsic(Intrinsic Id) {
  auto Idx = static_cast<size_t>(Id);
  return Idx < kCount && kSideEffect.Flags[Idx];
}

uint8_t intrinsicOutputCount(Intrinsic Id) {
  struct MultiOutTable {
    uint8_t Counts[kCount] = {};
    constexpr MultiOutTable() {
      auto MULTI_OUT = [this](Intrinsic IId, uint8_t C) {
        Counts[static_cast<size_t>(IId)] = C;
      };
#include "neverd/ir/intrinsics/intrinsics_multi_output.inc"
    }
  };
  static constexpr MultiOutTable Table;
  auto Idx = static_cast<size_t>(Id);
  return Idx < kCount ? Table.Counts[Idx] : 0;
}

Intrinsic intrinsicFromName(const char *Name) {
  if (!Name)
    return Intrinsic::None;
  if (std::strcmp(Name, "none") == 0)
    return Intrinsic::None;
  if (std::strcmp(Name, "syscall") == 0)
    return Intrinsic::Syscall;
  for (size_t I = 0; I < kCount; ++I) {
    auto IId = static_cast<Intrinsic>(I);
    if (auto *C = intrinsicCName(IId))
      if (std::strcmp(C, Name) == 0)
        return IId;
    if (auto *A = intrinsicAsmMnemonic(IId))
      if (std::strcmp(A, Name) == 0)
        return IId;
  }
  return Intrinsic::None;
}

} // namespace neverd
