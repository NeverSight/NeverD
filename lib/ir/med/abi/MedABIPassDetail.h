//===- MedABIPassDetail.h - Shared ABI-recovery slicing helpers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal data-flow and stack-offset tracing utilities shared between the
/// ABI recovery driver (MedABIPass.cpp) and its support routines
/// (MedABIPassSupport.cpp).  These back the per-call-site register/stack
/// argument scans in recoverCallAbi: resolving an indirect call target to a
/// constant address, mapping an outgoing store to its distance from the call
/// stack pointer, and finding the value reaching an argument register across
/// the CFG.
///
/// This header is an implementation detail of the med/ library and should NOT
/// be included by code outside lib/ir/med/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDABIPASSDETAIL_H
#define NEVERD_IR_MED_MEDABIPASSDETAIL_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace neverd {

struct BinaryImage;

//===----------------------------------------------------------------------===//
// Indirect-target resolution
//===----------------------------------------------------------------------===//

/// Resolve an indirect call target to a constant code address when the call
/// goes through a function pointer that provably holds a known function.
/// Returns the resolved address, or 0 when not provable.
va_t resolveIndirectTargetAddr(const MedBlock &Blk, int FromIdx,
                               const MedVar &V, int Depth);

/// Resolve an indirect-call target (or another value) back to the incoming
/// integer argument register from which it originated.  Follows the same
/// copy/cast and spill/reload chains as resolveIndirectTargetAddr.  nullopt
/// when the provenance is not provable within the call's block.
std::optional<int> resolveIndirectTargetArgIdx(const MedBlock &Blk, int FromIdx,
                                               const TargetRegInfo &TRI,
                                               const MedVar &V, bool IsWin64,
                                               int Depth = 0);

//===----------------------------------------------------------------------===//
// Stack-pointer offset tracing
//===----------------------------------------------------------------------===//

/// Offset of \p V relative to the function-entry stack pointer, following the
/// SP definition chain through constant add/sub decrements and width casts.
/// nullopt when \p V does not derive from the SP.
std::optional<int64_t> stackPtrDelta(const MedFunc &Func,
                                     const TargetRegInfo &TRI, const MedVar &V,
                                     int Depth = 0);

/// Whether \p V is, or derives from (through copies, width casts and a constant
/// add/sub), a frame register -- the stack OR frame pointer.
bool derivesFromFrameReg(const MedBlock &Blk, const TargetRegInfo &TRI,
                         const MedVar &V, int Depth = 0);

/// Identifies an SSA stack-pointer value (Id, version, register offset).
using SpOffsetKey = std::tuple<int, int, uint64_t>;

/// Records, for each stack-pointer value on the call-site SP's definition
/// chain, its byte offset *above* the call SP (call SP = 0).  Used to place
/// pushed arguments relative to the call SP when the absolute entry-relative
/// delta is unavailable.
void buildCallSpOffsets(const MedFunc &Func, const TargetRegInfo &TRI,
                        const MedVar &V, int64_t Off,
                        std::map<SpOffsetKey, int64_t> &Map, int Depth);

/// Offset of store-address \p V above the call SP, resolved against the call
/// SP's offset map.  nullopt when the address is not stack-pointer derived.
std::optional<int64_t> relStackOff(const MedFunc &Func,
                                   const TargetRegInfo &TRI, const MedVar &V,
                                   const std::map<SpOffsetKey, int64_t> &Map,
                                   int Depth);

//===----------------------------------------------------------------------===//
// Argument-register recovery
//===----------------------------------------------------------------------===//

/// Name of the callee reached by the branch relocation at \p InsnAddr (for a
/// relocatable object whose direct-call target operand is a placeholder), or an
/// empty string when no such relocation exists.
std::string relocCalleeName(const BinaryImage &Img, va_t InsnAddr);

/// The value an argument-register-defining op at index \p J contributes to its
/// slot, resolving a low-half sub-register sync to the paired full-width write
/// so a wide (pointer/struct) argument keeps its high bits.
MedVar argRegSourceValueInBlock(const MedBlock &Blk, int J,
                                const TargetRegInfo &TRI, bool IsWin64);

/// Select the authoritative PHI for integer argument slot \p ArgIdx in
/// \p Block.  A PHI with a value on every function-entry edge wins over an
/// alias that is undef on the first iteration; otherwise the widest register
/// view wins.  Centralizing this rule keeps in-block and cross-block argument
/// recovery independent of PHI insertion order.
const PhiNode *selectAuthoritativeArgPhi(const MedFunc &Func,
                                         const MedBlock &Block,
                                         const TargetRegInfo &TRI, int ArgIdx,
                                         bool IsWin64);

/// Value reaching argument register \p ArgIdx at the call in block \p BlockId,
/// found by walking the CFG backwards into predecessor blocks (nearest write,
/// then a block PHI, then -- when \p AllowUnknownLiveIn -- an incoming
/// parameter live-in).  \p FromLiveIn, when non-null, is set true if the value
/// came from the live-in fallback.  nullopt when the register is never set on
/// any path to the call.
std::optional<MedVar> findReachingArgReg(const MedFunc &Func,
                                         const TargetRegInfo &TRI, Arch TheArch,
                                         int BlockId, int ArgIdx, bool IsWin64,
                                         bool AllowUnknownLiveIn = false,
                                         bool *FromLiveIn = nullptr);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDABIPASSDETAIL_H
