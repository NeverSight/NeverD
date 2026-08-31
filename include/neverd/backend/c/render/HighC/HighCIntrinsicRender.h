//===- HighCIntrinsicRender.h - High IR intrinsic rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Renders HighIR intrinsic operations to C source.
///
/// Implementation split across:
///   HighCIntrinsicRender.cpp      — dispatch: MultiOutputRender,
///                                   renderIntrinsicCall
///   HighCIntrinsicRenderX86.cpp   — x86 multi-output & intrinsic rendering,
///                                   hiloCollapseExpr
///   HighCIntrinsicRenderARM.cpp   — ARM/AArch64 intrinsic rendering
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_HIGHC_HIGHCINTRINSICRENDER_H
#define NEVERD_BACKEND_C_RENDER_HIGHC_HIGHCINTRINSICRENDER_H
#include "neverd/Common.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <functional>
#include <string>
#include <vector>

namespace neverd {

using IsAliveFn = std::function<bool(const MedVar &)>;

struct MultiOutputRender {
  std::string operator()(Arch TheArch, Intrinsic IID,
                         const std::vector<MedVar> &Outputs,
                         const std::vector<ExprPtr> &Operands,
                         std::function<std::string(const HighExpr &)> ExprFn,
                         std::function<std::string(const MedVar &)> VarFn,
                         IsAliveFn IsAlive = {}) const;
};

//--- Dispatchers (HighCIntrinsicRender.cpp) ---
std::string renderIntrinsicCall(Intrinsic Id, Arch TheArch,
                                const std::vector<std::string> &Ops,
                                bool &HasCIntrinsics);

//--- Arch-specific (HighCIntrinsicRenderX86.cpp) ---
std::string
renderX86MultiOutput(Intrinsic IID, const std::vector<MedVar> &Outputs,
                     const std::vector<ExprPtr> &Operands,
                     std::function<std::string(const HighExpr &)> ExprFn,
                     std::function<std::string(const MedVar &)> VarFn,
                     IsAliveFn IsAlive);

std::string renderX86IntrinsicCall(Intrinsic Id,
                                   const std::vector<std::string> &Ops,
                                   bool &HasCIntrinsics);

/// Return the fail-closed diagnostic for an x86 intrinsic that cannot be
/// represented faithfully as standalone C, or nullptr when normal rendering
/// may proceed. renderX86IntrinsicCall uses this same policy.
const char *x86HighCIntrinsicFatalReason(Intrinsic Id);

/// Render an x86 intrinsic whose implicit memory operand is relative to FS/GS.
/// Returns an empty string for an unsupported intrinsic.  Recognized string
/// intrinsics with malformed architectural operands fail closed.
std::string renderX86SegmentedIntrinsicStatement(
    Arch TheArch, const HighExpr &Call, const HighExpr *PrimaryDst,
    std::function<std::string(const HighExpr &)> ExprFn,
    std::function<std::string(const MedVar &)> VarFn, IsAliveFn IsAlive = {});

const char *hiloCollapseExpr(Intrinsic Id);

/// Format a raw mnemonic + operands as an MSVC `__asm { ... }` statement.
std::string renderX86AsmStatement(const char *Mnemonic,
                                  const std::vector<std::string> &Ops);

//--- Arch-specific (HighCIntrinsicRenderARM.cpp) ---
std::string
renderARMMultiOutput(Intrinsic IID, const std::vector<MedVar> &Outputs,
                     const std::vector<ExprPtr> &Operands,
                     std::function<std::string(const HighExpr &)> ExprFn,
                     std::function<std::string(const MedVar &)> VarFn,
                     IsAliveFn IsAlive);

std::string renderARMIntrinsicCall(Intrinsic Id,
                                   const std::vector<std::string> &Ops,
                                   bool &HasCIntrinsics);

/// Format a raw mnemonic + operands as a GCC-style `__asm__ volatile(...)`
/// statement with register input constraints and a memory clobber.
std::string renderARMAsmStatement(const char *Mnemonic,
                                  const std::vector<std::string> &Ops);

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_HIGHC_HIGHCINTRINSICRENDER_H
