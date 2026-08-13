//===- SymDispatch.h - Reading the shape of a computed branch ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Works out what a jump through a computed address is actually indexing.
///
/// The usual way to recover a switch is to guess: pick the register that looks
/// like the index, run the dispatch code with the index set to nought, then to
/// one, then to two, and read the answers out until they stop looking like
/// code addresses.  It works, and everything it knows it knows by trying.
///
/// Executing the dispatch once with the index left as an unknown produces the
/// target as an expression instead, and the expression *is* the answer:
///
///     load(0x402000 + 8 * idx)
///
/// says the table is at 0x402000, that entries are eight bytes apart, that the
/// entry is the address, and that the index is whatever `idx` was seeded from
/// — none of it guessed, none of it needing a single concrete run.  The forms
/// that are not that shape say so as clearly, which matters more: a scaled
/// offset added to a base is a relative table, and anything else is not a
/// table at all.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMDISPATCH_H
#define NEVERD_SYMBOLIC_SYMDISPATCH_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/symbolic/SymState.h"

#include <cstdint>
#include <optional>

namespace neverd::symbolic {

/// How a table's entries turn into an address.
enum class DispatchKind : uint8_t {
  /// The entry is the address.
  Absolute,
  /// The entry is an offset from a base, which is how position-independent
  /// code and the compact AArch64 forms do it.
  Relative,
};

/// What a computed branch turned out to be indexing.
struct DispatchShape {
  DispatchKind Kind = DispatchKind::Absolute;
  /// Address of entry zero.
  uint64_t TableBase = 0;
  /// Bytes between one entry and the next.
  uint64_t EntryStride = 0;
  /// Bytes read per entry.
  uint16_t EntrySize = 0;
  /// The entry was sign-extended before use, so offsets can run backwards.
  bool EntryIsSigned = false;
  /// What a relative entry is measured from, and what it is scaled by first.
  /// The scale is one unless the target is `Base + entry * scale`, which is
  /// the shape of the compact tables that store a count of instructions.
  uint64_t RelativeBase = 0;
  uint64_t EntryScale = 1;
};

/// Read the shape of the branch \p Ops ends in.
///
/// \p IndexRegOffset names the register to leave unknown; everything else
/// starts out unknown too, so nothing has to be set up.  Returns nothing when
/// the branch is not a table dispatch, which includes the case where the
/// target does not depend on that register at all.
std::optional<DispatchShape>
analyzeDispatch(SymContext &Ctx, llvm::ArrayRef<LowOp> Ops,
                uint64_t IndexRegOffset, uint16_t IndexRegSize = 8,
                llvm::endianness ByteOrder = llvm::endianness::little);

/// Whether the branch \p Ops ends in can go anywhere at all as the register at
/// \p IndexRegOffset varies.
///
/// A narrower question than the shape, and a far more robust one: a shape can
/// go unread for a dozen reasons, but a target that does not mention the index
/// anywhere — including inside the address of everything it loaded on the way
/// — goes to one place, and whatever that is, it is not a switch.
///
/// The distinction matters because the alternative is to notice afterwards.
/// Running such a branch for index nought, one, two and so on yields the same
/// destination every time, and telling that from a switch whose cases happen
/// to share a body means guessing how long a run of repeats is too long.
///
/// Answers true when unsure, including whenever the run met an operation it
/// could not carry out — such an operation severs any dependence passing
/// through it, leaving something indistinguishable from independence.  The
/// asymmetry is deliberate: a wrong false discards a real dispatch, while a
/// wrong true only means carrying on and finding out.
bool dispatchVariesWithIndex(
    SymContext &Ctx, llvm::ArrayRef<LowOp> Ops, uint64_t IndexRegOffset,
    uint16_t IndexRegSize = 8,
    llvm::endianness ByteOrder = llvm::endianness::little);

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMDISPATCH_H
