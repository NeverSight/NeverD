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

#include <cstdint>
#include <vector>

namespace neverd {

struct BinaryImage;
struct TargetRegInfo;

/// Registers that survive a call under \p Img's target ABI.  Shared by
/// constant-folding and path-emulation fallbacks.  Defined in
/// JumpTableResolverFold.cpp.
std::vector<uint64_t> callPreservedRegs(const BinaryImage &Img);

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
uint64_t scaledIndexReg(const std::vector<LowOp> &Ops, int FromIdx, NdVar V);

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
/// entry count of an absolute-pointer jump table.  Defined in
/// JumpTableResolverBounds.cpp.
uint32_t countCodePtrRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint16_t EntrySize);

/// Count consecutive PC-relative-to-code relocation slots from \p TableAddr --
/// the entry count of a PIC `switch` jump table.  Defined in
/// JumpTableResolverBounds.cpp.
uint32_t countRelCodeRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint16_t EntrySize);

/// Truncate a RelCodeReloc entry \p Run so it stops at the next PIC jump-table
/// base anchor after \p BaseAddr.  Defined in JumpTableResolverBounds.cpp.
uint32_t boundRelRunByNextAnchor(const BinaryImage &Img, va_t BaseAddr,
                                 uint16_t EntrySize, uint32_t Run);

/// Resolve an address nd-var to a stack/frame slot key (frame register plus a
/// constant byte offset); false for any non-frame or scaled-index address.
/// Defined in JumpTableResolverSlice.cpp.
bool frameSlotKey(const std::vector<LowOp> &Ops, int FromIdx, NdVar AddrV,
                  const TargetRegInfo &TRI, uint64_t &BaseReg, int64_t &Off);

//===----------------------------------------------------------------------===//
// Table entry decoding
//===----------------------------------------------------------------------===//

/// Decode a single table entry into a target address.  For the compact-table
/// form (TargetBase != 0) the target is `TargetBase + entry * Scale`; otherwise
/// relative tables use `BaseAddr + entry` and absolute tables store the target.
/// Defined in JumpTableResolverTargets.cpp.
va_t decodeTableEntry(const uint8_t *P, uint16_t EntrySize, bool IsRelative,
                      bool IsSigned, va_t BaseAddr, va_t TargetBase = 0,
                      uint32_t Scale = 1);

} // namespace neverd

#endif // NEVERD_IR_LOW_JUMPTABLE_JUMPTABLERESOLVERDETAIL_H
