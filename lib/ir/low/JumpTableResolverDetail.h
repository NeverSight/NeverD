//===- JumpTableResolverDetail.h - Shared backward-slicing helpers -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal data-flow slicing utilities shared between the jump-table
/// resolver's translation units: the architecture-generic framework
/// (JumpTableResolver.cpp), the guard/bounds analysis
/// (JumpTableResolverGuards.cpp), the table base-address detectors
/// (JumpTableResolverSource.cpp), and the ARM-family target detectors
/// (JumpTableResolverARM.cpp).
///
/// This header is an implementation detail of the low/ library and should NOT
/// be included by code outside lib/ir/low/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_JUMPTABLERESOLVERDETAIL_H
#define NEVERD_IR_LOW_JUMPTABLERESOLVERDETAIL_H

#include "neverd/Common.h"
#include "neverd/ir/low/LowIR.h"

#include <cstdint>
#include <vector>

namespace neverd {

struct BinaryImage;
struct TargetRegInfo;

//===----------------------------------------------------------------------===//
// Backward data-flow slicing
//===----------------------------------------------------------------------===//

/// Reaching-definition index of \p V searching backward from \p FromIdx, or -1.
int reachingDefIdx(const std::vector<LowOp> &Ops, int FromIdx,
                   const NdVar &V);

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
/// ZEXT/SEXT, low-half SUBBYTES/CONCAT) backward to the earliest register in the
/// chain, or \p RegOff if none is found.  Defined in
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

/// Count consecutive code-pointer relocation slots from \p TableAddr -- the
/// entry count of an absolute-pointer jump table.  Defined in
/// JumpTableResolver.cpp.
uint32_t countCodePtrRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint16_t EntrySize);

/// Resolve an address nd-var to a stack/frame slot key (frame register plus a
/// constant byte offset); false for any non-frame or scaled-index address.
/// Defined in JumpTableResolver.cpp.
bool frameSlotKey(const std::vector<LowOp> &Ops, int FromIdx, NdVar AddrV,
                  const TargetRegInfo &TRI, uint64_t &BaseReg, int64_t &Off);

} // namespace neverd

#endif // NEVERD_IR_LOW_JUMPTABLERESOLVERDETAIL_H
