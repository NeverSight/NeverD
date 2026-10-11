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
///                                   vector intrinsics by element kind,
///                                   `__fastfail`, GS/FS reads,
///                                   hiloCollapseExpr
///   HighCIntrinsicRenderARM.cpp   — ARM/AArch64 intrinsic rendering
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_HIGHC_HIGHCINTRINSICRENDER_H
#define NEVERD_BACKEND_C_RENDER_HIGHC_HIGHCINTRINSICRENDER_H
#include "neverd/Common.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/ArrayRef.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace neverd {

using IsAliveFn = std::function<bool(const MedVar &)>;
using SameWidthUnsignedFn = std::function<bool(const HighExpr &, uint16_t)>;

struct MultiOutputRender {
  std::string operator()(Arch TheArch, Intrinsic IID,
                         const std::vector<MedVar> &Outputs,
                         const std::vector<ExprPtr> &Operands,
                         std::function<std::string(const HighExpr &)> ExprFn,
                         std::function<std::string(const MedVar &)> VarFn,
                         IsAliveFn IsAlive = {}) const;
};

//--- Dispatchers (HighCIntrinsicRender.cpp) ---
/// \p GnuToolchain: the output is compiled by GCC or Clang rather than MSVC,
/// so an MSVC-only intrinsic name has no declaration.  \p OperandBytes, when
/// given, is the width of each operand in \p Ops.
std::string renderIntrinsicCall(Intrinsic Id, Arch TheArch,
                                const std::vector<std::string> &Ops,
                                uint16_t ResultBytes, bool &HasCIntrinsics,
                                bool GnuToolchain = false,
                                llvm::ArrayRef<uint16_t> OperandBytes = {});

//--- Arch-specific (HighCIntrinsicRenderX86.cpp) ---
/// The C helpers an x87 value prints through.
enum class X87CHelper : uint8_t {
  Frndint,     ///< frndint, rounding to an integer by the x87 control word
  Fsqrt,       ///< fsqrt, the correctly rounded square root
  ControlWord, ///< fnstcw, the unit's control word
  Fprem,       ///< fprem, fprem1 and the status word they leave
#define NEVERD_X87_VALUE_HELPER(Intrinsic, Name, Asm, Operands, PopsST1)       \
  Intrinsic,
#include "neverd/ir/intrinsics/X87ValueInstructions.def"
};
const char *x87CHelperName(X87CHelper Helper);
/// The helper that runs the x87 value intrinsic \p Id, if it is one.
std::optional<X87CHelper> x87ValueHelper(Intrinsic Id);
/// Whether \p V is the x87 control word of an \p TheArch function.
bool isX87ControlWord(Arch TheArch, const MedVar &V);
/// Write what the x87 values of a unit need: when \p UsesExtended, the
/// assertions that the compiler's `long double` is the x87 extended format
/// with the size of the `unsigned _BitInt(80)` its bits travel in, then each
/// helper in \p Used.
void writeX87CHelpers(llvm::raw_ostream &OS, bool UsesExtended,
                      const std::set<X87CHelper> &Used);

std::string
renderX86MultiOutput(Intrinsic IID, const std::vector<MedVar> &Outputs,
                     const std::vector<ExprPtr> &Operands,
                     std::function<std::string(const HighExpr &)> ExprFn,
                     std::function<std::string(const MedVar &)> VarFn,
                     IsAliveFn IsAlive);

std::string renderX86IntrinsicCall(Intrinsic Id,
                                   const std::vector<std::string> &Ops,
                                   bool &HasCIntrinsics,
                                   bool GnuToolchain = false);

/// Render x86 intrinsics whose C spelling depends on the complete HighIR
/// result and operand types. Returns an empty string when \p Call is not one
/// of those intrinsics; recognized malformed calls fail closed.
std::string renderX86TypedIntrinsicCall(
    Arch TheArch, const HighExpr &Call,
    std::function<std::string(const HighExpr &)> ExprFn, bool &HasCIntrinsics,
    bool GnuToolchain,
    std::function<std::string(Intrinsic, unsigned)> FPHelperName = {});

/// The lane width, in bits, of an x86 saturating lane operation
/// (X86SaturatingLanes.def) whose C helper \p Call needs, or nullopt when
/// \p Call is no such operation of a 1- or 2-byte lane.
std::optional<unsigned> x86SaturatingLaneBits(const HighExpr &Call);

/// The name to give the helper of saturating lane operation \p Id on
/// \p Bits-bit lanes (`neverd_adds_i8`).
std::string x86SaturatingLaneHelperName(Intrinsic Id, unsigned Bits);

/// Write the helper \p Name of saturating lane operation \p Id on \p Bits-bit
/// lanes: it computes in int32_t and clamps to the lane's range.
void writeX86SaturatingLaneHelper(llvm::raw_ostream &OS,
                                  const std::string &Name, Intrinsic Id,
                                  unsigned Bits);

/// \p Call, a saturating lane operation, as a call of its helper \p Helper
/// on its operands cast to the lane type.
std::string renderX86SaturatingLane(
    const HighExpr &Call, const std::string &Helper,
    const std::function<std::string(const HighExpr &)> &ExprFn);

/// Return the fail-closed diagnostic for an x86 intrinsic that cannot be
/// represented faithfully as standalone C, or nullptr when normal rendering
/// may proceed. renderX86IntrinsicCall uses this same policy.
const char *x86HighCIntrinsicFatalReason(Intrinsic Id);

/// Whether a flat x86 memory intrinsic renders through an <immintrin.h>
/// intrinsic rather than inline assembly.
bool x86MemoryIntrinsicUsesCHeader(Intrinsic Id);

/// Whether an x86 intrinsic may render through an <intrin.h> declaration on
/// a Windows target: REP MOVS/STOS, the flat LIDT/SIDT/INVLPG forms and an
/// FS/GS MXCSR transfer through the segment accessors.
bool x86UsesMsvcIntrinsicHeader(Intrinsic Id);

/// Whether an x86 intrinsic reads fixed registers and prints as an `__asm`
/// block that loads them: the hypercalls, MONITOR/MWAIT and XSETBV.
bool x86UsesImplicitRegisterAsm(Intrinsic Id);

/// Whether an x86 intrinsic needs <x86intrin.h> on a GCC or Clang target.
/// Older Clang declares _m_prefetchw only there, not in <immintrin.h>.
bool x86UsesGnuIntrinsicHeader(Intrinsic Id);

/// Render an x86 intrinsic that needs full typed-statement context, including
/// architectural preconditions and implicit memory relative to FS/GS. Returns
/// an empty string for an unsupported intrinsic. Recognized intrinsics with
/// malformed architectural operands fail closed.
std::string renderX86SegmentedIntrinsicStatement(
    Arch TheArch, const HighExpr &Call, const HighExpr *PrimaryDst,
    std::function<std::string(const HighExpr &)> ExprFn,
    std::function<std::string(const MedVar &)> VarFn, IsAliveFn IsAlive = {},
    SameWidthUnsignedFn SameWidthUnsigned = {}, bool MsvcIntrinsics = false);

/// Windows `int 0x29` / `__fastfail`.  True when \p E is that intrinsic.
bool isX86FastFailCall(const HighExpr &E);

/// A value-returning x86 `int imm8`, the x64 `int 2Dh` debug service, or an
/// instruction x86UsesImplicitRegisterAsm names, as an `__asm` block that
/// loads its register inputs and, when \p ResultVar is not empty, moves the
/// result register into it.  Empty when \p Call is none of those.
std::string renderX86InterruptStatement(
    Arch TheArch, const HighExpr &Call, llvm::StringRef ResultVar,
    unsigned ResultSize, std::function<std::string(const HighExpr &)> ExprFn);

/// MSVC `<intrin.h>` GS/FS scalar load, or empty when this access is not that
/// x86 form.
std::string renderX86MsvcSegmentedLoad(Arch TheArch, unsigned SizeBytes,
                                       llvm::StringRef Addr,
                                       NdMemoryOrdering Ordering,
                                       NdMemoryAddressSpace AddressSpace);

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
                                   uint16_t ResultBytes, bool &HasCIntrinsics);

/// An ACLE vector intrinsic whose C name ends in its element kind
/// (`vaeseq_u8`, `vmul_p8`) and whose result is that vector: each operand
/// \p OperandBytes gives as wide as the vector converts to the vector type,
/// and the result back.  Empty for any other intrinsic.
std::string renderNeonVectorIntrinsic(Intrinsic Id,
                                      const std::vector<std::string> &Ops,
                                      llvm::ArrayRef<uint16_t> OperandBytes,
                                      uint16_t ResultBytes,
                                      bool &HasCIntrinsics);

/// Format a raw mnemonic + operands as a GCC-style `__asm__ volatile(...)`
/// statement with register input constraints and a memory clobber.
std::string renderARMAsmStatement(const char *Mnemonic,
                                  const std::vector<std::string> &Ops);

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_HIGHC_HIGHCINTRINSICRENDER_H
