//===- LLVMCIntrinsicRender.h - LLVM intrinsic rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Renders LLVM intrinsic calls to inline assembly or ACLE C source.
///
/// Implementation split across:
///   LLVMCIntrinsicRender.cpp      — arch-dispatching: renderInlineAsm,
///                                   renderFence, renderDebugBreak
///   LLVMCIntrinsicRenderX86.cpp   — x86 inline-asm, `__fastfail`, GS/FS loads
///   LLVMCIntrinsicRenderARM.cpp   — ARM/AArch64 inline-asm rendering & table
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_LLVMC_LLVMCINTRINSICRENDER_H
#define NEVERD_BACKEND_C_RENDER_LLVMC_LLVMCINTRINSICRENDER_H
#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/AtomicOrdering.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class LoadInst;
class CallBase;
class CallInst;
class Value;
} // namespace llvm

namespace neverd {

struct AsmToCEntry {
  const char *AsmStr;
  const char *CName;
};

struct InlineAsmRender {
  std::string Code;
  bool SetIntrinsics = false;
};

// A deliberately narrow contract for the unused-output STOS assembly emitted
// by MedLLVMX86ValueEmitter. The address/count register updates are already
// represented separately in the lifted IR.
struct X86RepStos {
  enum Direction { Forward, Backward, Dynamic } Dir;
  unsigned ElementBytes;
  unsigned AddressBits;
};
std::optional<X86RepStos> classifyX86RepStos(Arch TheArch,
                                             const llvm::CallInst &Call);

// The same contract for MOVS, whose source, destination and count register
// updates the lifted IR represents separately too.
struct X86RepMovs {
  enum Direction { Forward, Backward, Dynamic } Dir;
  unsigned ElementBytes;
  unsigned AddressBits;
};
std::optional<X86RepMovs> classifyX86RepMovs(Arch TheArch,
                                             const llvm::CallInst &Call);

//--- Arch-specific (LLVMCIntrinsicRenderX86.cpp) ---
/// Render the native RDTSCP pair without losing its auxiliary result or read.
std::optional<InlineAsmRender>
renderX86TimestampCall(Arch TheArch, const llvm::CallBase &Call,
                       const std::string &ResultName, bool ResultLive);
/// Recognize the complete scalar MSR/port call with ordered native effects.
bool isX86DescriptorStateNativeName(llvm::StringRef Name);
std::optional<InlineAsmRender>
renderX86DescriptorStateCall(Arch TheArch, BinaryFormat Format,
                             const llvm::CallBase &Call,
                             const std::string &ResultName, bool ResultLive,
                             const std::vector<std::string> &Args);

bool isX86ScalarSystemNativeName(llvm::StringRef Name);
std::optional<InlineAsmRender>
renderX86ScalarSystemCall(Arch TheArch, BinaryFormat Format,
                          const llvm::CallBase &Call,
                          const std::string &ResultName, bool ResultLive,
                          const std::vector<std::string> &Args);
/// Recognize the complete owned CR/DR call, rejecting malformed candidates.
bool isX86SystemRegisterNativeName(llvm::StringRef Name);
std::optional<InlineAsmRender>
renderX86SystemRegisterCall(Arch TheArch, BinaryFormat Format,
                            const llvm::CallBase &Call,
                            const std::string &ResultName, bool ResultLive,
                            const std::vector<std::string> &Args);
/// Preserve the exact prefetch hint, address and segment of owned assembly.
std::optional<InlineAsmRender>
renderX86PrefetchCall(Arch TheArch, const llvm::CallBase &Call,
                      const std::vector<std::string> &Args);
const char *lookupX86AsmToC(const char *Mnem);
InlineAsmRender renderX86InlineAsm(Arch TheArch, const std::string &AsmStr,
                                   const std::string &Mnemonic,
                                   bool IsStructReturn,
                                   const std::string &ResultName,
                                   bool ResultLive,
                                   const std::vector<std::string> &Args);
std::string renderX86Fence(llvm::AtomicOrdering Ordering);
const char *renderX86DebugBreak(BinaryFormat Format);
bool isX86FastFailName(llvm::StringRef Name);
std::string renderX86SegmentedLoad(
    Arch TheArch, const llvm::LoadInst &LI,
    const std::function<std::string(const llvm::Value *)> &ValueStr);

//--- Arch-specific (LLVMCIntrinsicRenderARM.cpp) ---
/// False for another assembly identity; malformed owned calls throw.
bool validateAArch64AddressMemoryCall(Arch TheArch, const llvm::CallInst &Call);
/// Validate the original GPR/SIMD writeback call before source filtering.
bool validateAArch64WritebackMemoryCall(Arch TheArch,
                                        const llvm::CallInst &Call);
std::optional<InlineAsmRender>
renderAArch64AddressMemoryCall(Arch TheArch, const llvm::CallInst &Call,
                               const std::string &ResultName, bool ResultLive,
                               const std::vector<std::string> &Args);
std::optional<InlineAsmRender>
renderAArch64WritebackMemoryCall(Arch TheArch, const llvm::CallInst &Call,
                                 const std::string &ResultName, bool ResultLive,
                                 const std::vector<std::string> &Args);
std::optional<InlineAsmRender>
renderAArch64PAuthLoadCall(Arch TheArch, const llvm::CallInst &Call,
                           const std::string &ResultName, bool ResultLive,
                           const std::vector<std::string> &Args);
const char *lookupArmAsmToC(const char *Mnem);
InlineAsmRender renderARMInlineAsm(Arch TheArch, const std::string &AsmStr,
                                   const std::string &Mnemonic,
                                   const std::string &ResultName,
                                   bool ResultLive,
                                   const std::vector<std::string> &Args);
std::string renderARMFence(llvm::AtomicOrdering Ordering);
const char *renderARMDebugBreak();

//--- Dispatchers (LLVMCIntrinsicRender.cpp) ---
InlineAsmRender renderInlineAsm(Arch TheArch, const std::string &AsmStr,
                                bool IsStructReturn,
                                const std::string &ResultName, bool ResultLive,
                                const std::vector<std::string> &Args);

std::string renderFence(Arch TheArch, llvm::AtomicOrdering Ordering);

/// Returns the arch-appropriate debug-break statement (with trailing newline):
/// x86's is Microsoft's `__debugbreak` in a COFF unit and the GNU builtin in
/// any other, whose headers do not declare it.
const char *renderDebugBreak(Arch TheArch, BinaryFormat Format);

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_LLVMC_LLVMCINTRINSICRENDER_H
