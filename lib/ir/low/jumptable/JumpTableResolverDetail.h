//===- JumpTableResolverDetail.h - Shared backward-slicing helpers -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal data-flow slicing utilities shared between the jump-table
/// resolver's translation units.  The architecture-generic framework is split
/// by responsibility across strategy dispatch (JumpTableResolver.cpp),
/// backward slicing (JumpTableResolverSlice.cpp), table-base constant folding
/// (JumpTableResolverFold.cpp), index normalization
/// (JumpTableResolverNorm.cpp), guard-free entry-count bounds
/// (JumpTableResolverBounds.cpp), and entry decoding with target validation
/// (JumpTableResolverTargets.cpp).  The guard/bounds analysis spans
/// JumpTableResolverGuards.cpp, JumpTableResolverGuardRange.cpp,
/// JumpTableResolverGuardAlias.cpp, and JumpTableResolverGuardCFG.cpp;
/// emulation fallbacks live in JumpTableResolverEmu.cpp; the table
/// source/stack/shape detectors in JumpTableResolverSource.cpp,
/// JumpTableResolverStack.cpp, JumpTableResolverShapes.cpp, and
/// JumpTableResolverTwoLevel.cpp; and the ARM-family target detectors in
/// JumpTableResolverARM.cpp.
///
/// This header is an implementation detail of the low/ library's jump-table
/// resolver and should NOT be included by code outside
/// lib/ir/low/jumptable/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_JUMPTABLE_JUMPTABLERESOLVERDETAIL_H
#define NEVERD_IR_LOW_JUMPTABLE_JUMPTABLERESOLVERDETAIL_H

#include "neverd/Common.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

struct BinaryImage;
struct RelocatedAddressField;
struct TargetRegInfo;

/// True only when one exact LOAD occurrence eligible to provide a jump-table
/// target belongs to the ordinary process-image address space.  Segment
/// offsets may numerically collide with image VAs but can never authenticate
/// image jump-table storage.
bool jumpTableTargetLoadUsesDefaultAddressSpace(
    llvm::ArrayRef<LowOp> Ops, va_t Address, int Sequence,
    const NdVar &Output);

/// Checked signed frame-offset arithmetic used by stack-table proofs.  A
/// failure is evidence incompleteness, never a wrapping offset.
std::optional<int64_t> stackCheckedOffset(int64_t Base, int64_t Delta,
                                          bool Subtract = false);

/// Apply a signed frame delta to a guest virtual address without host signed
/// casts or modular wraparound.
std::optional<va_t> checkedVAOffset(va_t Base, int64_t Delta);

/// Return the unique immutable object-data owner of the complete byte span
/// [Start, Start + Size), or nullopt when the span is empty, wraps, crosses an
/// exact section/segment owner, reaches executable or runtime-writable bytes,
/// or does not match \p ExpectedOwner.  Unlike
/// BinaryImage::relocatedTargetBelongsToOwner, this is a memory-access proof:
/// a data owner's legal one-past address is deliberately not dereferenceable.
std::optional<va_t>
exactImmutableDataSpanOwner(const BinaryImage &Img, va_t Start, uint64_t Size,
                            va_t ExpectedOwner = InvalidVA);

/// Registers that survive a call under \p Img's target ABI.  Shared by
/// constant-folding and path-emulation fallbacks.  Defined in
/// JumpTableResolverFold.cpp.
std::vector<uint64_t> callPreservedRegs(const BinaryImage &Img);

/// True when exception/unwind metadata links \p Target's cold/chained range
/// to the exact primary function at \p FunctionEntry.  Shared by ordinary
/// switch-target validation and opaque mixed-table identity classification so
/// the two paths cannot drift on out-of-line local fragments.
bool isExplicitlyOwnedFunctionFragment(const BinaryImage &Img,
                                       va_t FunctionEntry, va_t Target);

namespace detail {

/// Immutable proof-context portion of a target-role certificate.  An absent
/// active-root override is semantically distinct from a present empty set:
/// the former falls back to persistent CFG roots, while the latter explicitly
/// authorizes no roots.  Consumer-audit mode also changes the proof graph and
/// therefore participates in exact cache identity even when the effective
/// root addresses happen to match.
struct TargetRoleProofContextKey {
  bool ProofContextComplete = false;
  bool HasActiveProofRoots = false;
  bool ConsumerAuditMode = false;
  std::vector<va_t> ProofRoots;

  bool operator==(const TargetRoleProofContextKey &Other) const = default;
};

inline bool targetRoleProofContextMatches(
    const TargetRoleProofContextKey &Key, bool ProofContextComplete,
    bool HasActiveProofRoots, bool ConsumerAuditMode,
    const std::set<va_t> &ProofRoots) {
  return Key.ProofContextComplete == ProofContextComplete &&
         Key.HasActiveProofRoots == HasActiveProofRoots &&
         Key.ConsumerAuditMode == ConsumerAuditMode &&
         Key.ProofRoots.size() == ProofRoots.size() &&
         std::equal(Key.ProofRoots.begin(), Key.ProofRoots.end(),
                    ProofRoots.begin());
}

} // namespace detail

//===----------------------------------------------------------------------===//
// Backward data-flow slicing
//===----------------------------------------------------------------------===//

/// Reaching-definition index of \p V searching backward from \p FromIdx, or -1.
int reachingDefIdx(const std::vector<LowOp> &Ops, int FromIdx, const NdVar &V);

/// Trace a nd-var backward through COPY chains to a plain register (InvalidVA
/// if it does not resolve to one).
uint64_t traceToRegister(const std::vector<LowOp> &Ops, int FromIdx, NdVar V);

/// Trace a register back through reaching value-preserving definitions (COPY,
/// ZEXT/SEXT, low-half SUBBYTES) to its ultimate source register.
uint64_t traceRegSource(const std::vector<LowOp> &Ops, int FromIdx,
                        uint64_t RegOff);

/// If a nd-var is a scaled index (traced through COPY/ZEXT/SEXT to an
/// INT_MULT(reg, const>1) or INT_LEFT(reg, const)), return the source index
/// register; otherwise InvalidVA.
uint64_t scaledIndexReg(const std::vector<LowOp> &Ops, int FromIdx, NdVar V,
                        NdVar *IndexValue = nullptr,
                        va_t *IndexUseAddr = nullptr,
                        int *IndexUseSeq = nullptr);

/// Follow a quasi-copy chain (COPY, power-of-two INT_AND, INT_OR of upper bits,
/// ZEXT/SEXT, low-half SUBBYTES/CONCAT) backward to the earliest register in
/// the chain, or \p RegOff if none is found.  Defined in
/// JumpTableResolverGuards.cpp.
uint64_t quasiCopySource(const std::vector<LowOp> &Ops, int StartIdx,
                         uint64_t RegOff);

//===----------------------------------------------------------------------===//
// Bound and reloc-run helpers
//===----------------------------------------------------------------------===//

/// Scan \p Ops for the tightest range-guard bound on the index register (or any
/// index when \p IndexReg is InvalidVA), never below \p Current.  Defined in
/// JumpTableResolverGuards.cpp.
uint32_t findBestBound(const std::vector<LowOp> &Ops, uint32_t Current,
                       uint64_t IndexReg = InvalidVA);

/// True when the guard \p Op constrains the table index register, so that
/// unrelated masks are not mistaken for the table bound.  Defined in
/// JumpTableResolverGuards.cpp.
bool guardConstrainsIndex(const std::vector<LowOp> &RecOps, const LowOp &Op,
                          uint64_t IndexReg);

/// Read a constant operand, seeing through a -O0 const-into-temp/reg
/// materialization (`COPY t,#k; cmp x,t`).  Defined in
/// JumpTableResolverGuards.cpp.
bool resolveConstThroughCopy(const std::vector<LowOp> &Ops, int Before,
                             const NdVar &V, uint64_t &Out);

/// Count consecutive code-pointer relocation slots from \p TableAddr -- the
/// bounded entry count of an absolute-pointer jump table.  \p ScanComplete is
/// false only when another slot exists beyond the global scan limit.  Defined
/// in JumpTableResolverBounds.cpp.
uint32_t countCodePtrRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint64_t EntryStride,
                              bool *ScanComplete = nullptr);

/// One candidate-local exemption for an address materialization that directly
/// names immutable bytes copied into a stack-resident jump table.  Keeping the
/// field, target, owner, effective source span, and candidate base together
/// prevents an adjusted pointer to a neighbouring table from erasing that
/// table's independent boundary anchor.
struct AuthenticatedSourceAnchorExemption {
  va_t CandidateBaseVA = InvalidVA;
  va_t FieldVA = InvalidVA;
  va_t TargetVA = InvalidVA;
  va_t TargetOwnerVA = InvalidVA;
  va_t EffectiveSourceVA = InvalidVA;
  uint64_t SourceByteCount = 0;

  bool
  operator==(const AuthenticatedSourceAnchorExemption &Other) const = default;
};

/// Validate an exemption against the exact loader field and the current raw
/// relocation run.  In particular, TargetVA must directly equal the effective
/// source address; deriving source A from an occurrence that names adjacent B
/// does not make B a consumer of A's table.
bool authenticatedSourceAnchorExemptionMatches(
    const AuthenticatedSourceAnchorExemption &Exemption, va_t BaseAddr,
    uint64_t EntryStride, uint32_t Run, va_t FieldVA,
    const RelocatedAddressField &Field);

/// Validate an exemption backed by one exact relocation-free address
/// occurrence.  Unlike the loader-field overload, this form deliberately has
/// no FieldVA: its authority is the complete AArch64 materialization and
/// immediate immutable dereference certificate carried by \p Occurrence.
bool authenticatedSourceAnchorExemptionMatches(
    const AuthenticatedSourceAnchorExemption &Exemption, va_t BaseAddr,
    uint64_t EntryStride, uint32_t Run,
    const RelocatedInstructionAddressOccurrence &Occurrence);

/// Truncate an absolute code-pointer relocation run at the next independently
/// materialized table-base anchor.  This prevents two adjacent absolute tables
/// from being conflated into one physical owner.
uint32_t boundCodePtrRunByNextAnchor(
    const BinaryImage &Img, va_t BaseAddr, uint64_t EntryStride, uint32_t Run,
    const std::set<va_t> &DecodedAnchors,
    const std::map<va_t, AuthenticatedSourceAnchorExemption>
        &AuthenticatedSources = {});

/// True when the end of an absolute relocation run is independently bounded
/// by the mapped storage owner or another materialized table-base anchor.
bool codePtrRelocRunHasExactBoundary(const BinaryImage &Img, va_t BaseAddr,
                                     uint64_t EntryStride, uint32_t Run,
                                     const std::set<va_t> &DecodedAnchors);

/// Count consecutive PC-relative-to-code relocation slots from \p TableAddr --
/// the bounded entry count of a PIC `switch` jump table.  \p ScanComplete is
/// false only when another slot exists beyond the global scan limit.  Defined
/// in JumpTableResolverBounds.cpp.
uint32_t countRelCodeRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint64_t EntryStride,
                              bool *ScanComplete = nullptr);

/// Truncate a RelCodeReloc entry \p Run so it stops at the next PIC jump-table
/// base anchor after \p BaseAddr.  Defined in JumpTableResolverBounds.cpp.
uint32_t boundRelRunByNextAnchor(const BinaryImage &Img, va_t BaseAddr,
                                 uint64_t EntryStride, uint32_t Run,
                                 const std::set<va_t> &DecodedAnchors);

/// Apply LowIR's integer-coercion contract to an INT_AND mask: both operands
/// are zero-extended/truncated to OutputSize before the operation, and a
/// narrower dynamic operand cannot set newly introduced high bits.  Returns
/// the effective set-bit mask, or nullopt for an unsupported width.
std::optional<uint64_t> effectiveIntegerAndMask(uint64_t EncodedMask,
                                                uint16_t MaskSize,
                                                uint16_t DynamicSize,
                                                uint16_t OutputSize);

/// Apply the resolver's case-label affine mapping in the 64-bit modular guest
/// domain.  This is the bit-pattern form of
/// `(EntryIndex * Stride << NormShift) + NormBase`; nullopt rejects an
/// unrepresentable shift instead of invoking host signed-shift overflow.
std::optional<uint64_t> recoverCaseLabelBitPattern(uint64_t EntryIndex,
                                                   uint32_t Stride,
                                                   uint32_t NormShift,
                                                   int64_t NormBase);

/// Evaluate one primitive used by the precise jump-table guard proof.  Inputs
/// carry their original LowIR widths because comparisons and arithmetic follow
/// the emitter's integer-coercion contract rather than the host uint64_t
/// width.  Exposed only through this internal header so semantic differential
/// tests can keep the resolver evaluator aligned with production emission.
std::optional<uint64_t>
evaluateJumpTableGuardPrimitive(NdOp Opcode, uint16_t OutputSize,
                                llvm::ArrayRef<uint64_t> Inputs,
                                llvm::ArrayRef<uint16_t> InputSizes);

/// Translate one integer LowIR operation to the shared symbolic bit-vector
/// semantics used by complete-domain jump-table proofs.  Inputs are coerced
/// exactly as the production emitter coerces them; unsupported or
/// architecture-dependent width combinations return nullopt rather than
/// authorizing a range from an approximate expression.
std::optional<symbolic::SymRef>
symbolizeJumpTableIntegerOperation(symbolic::SymContext &Ctx, NdOp Opcode,
                                   uint16_t OutputSize,
                                   llvm::ArrayRef<symbolic::SymRef> RawInputs);

/// True when one relative-target transform operates in the complete guest
/// pointer domain.  A mere widening event is not enough: scale and anchor
/// operations must consume and produce pointer-width values, and the anchor
/// operand itself must be pointer-width as well.
bool relativeTargetTransformUsesPointerWidth(NdOp Opcode,
                                             uint16_t DynamicInputSize,
                                             uint16_t OtherInputSize,
                                             uint16_t OutputSize,
                                             uint16_t PointerSize);

/// Resolve an address nd-var to a stack/frame slot key (frame register plus a
/// constant byte offset); false for any non-frame or scaled-index address.
/// Defined in JumpTableResolverSlice.cpp.
bool frameSlotKey(const std::vector<LowOp> &Ops, int FromIdx, NdVar AddrV,
                  const TargetRegInfo &TRI, uint64_t &BaseReg, int64_t &Off);

//===----------------------------------------------------------------------===//
// Table entry decoding
//===----------------------------------------------------------------------===//

/// Decode a single table entry into a target address.  When HasTargetBase is
/// set, the target is `TargetBase + entry * Scale`; otherwise relative tables
/// use `BaseAddr + entry` and absolute tables store the target.  Arithmetic is
/// performed modulo AddressBytes so 32-bit wrapped tables match execution.
/// Defined in JumpTableResolverTargets.cpp.
std::optional<va_t> decodeTableEntry(const uint8_t *P, uint16_t EntrySize,
                                     bool IsRelative, bool IsSigned,
                                     va_t BaseAddr, bool HasTargetBase = false,
                                     va_t TargetBase = 0, uint32_t Scale = 1,
                                     uint16_t AddressBytes = 8);

/// Canonicalize a serialized absolute code pointer for the image's uniform
/// instruction mode.  ARM bit 0 is a mode tag, not part of the decode VA;
/// mixed ARM/Thumb targets are rejected until CFG identity is keyed by
/// (address, mode).  Relative and compact table entries must not use this.
std::optional<va_t> canonicalizeAbsoluteTableCodeTarget(const BinaryImage &Img,
                                                        va_t RawTarget);

} // namespace neverd

#endif // NEVERD_IR_LOW_JUMPTABLE_JUMPTABLERESOLVERDETAIL_H
