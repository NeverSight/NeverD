//===- BitBlaster.h - Bitvector expressions down to clauses -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lowers a \c SymExpr bitvector expression to a boolean circuit, one literal
/// per bit.
///
/// Every operator in the expression language is a finite circuit over the bits
/// of its operands: an addition is a chain of full adders, a comparison is a
/// chain that walks from the most significant bit down, a shift by a value
/// that is not known is a ladder of multiplexers, a multiplication is an array
/// of shifted partial products.  Writing those circuits out and handing them
/// to a satisfiability engine decides any question about the expression
/// exactly, at any width, with no abstraction to refine and no interval to
/// widen — which is the property NeverD needs, because the questions it asks
/// are about obfuscated arithmetic where every bit of the answer matters.
///
/// The cost is that the circuit is as large as the arithmetic is wide, and for
/// multiplication and division it grows with the square of the width.  That is
/// paid for in three ways.  Constants fold as the circuit is built, so a
/// literal operand costs nothing.  Gates are shared structurally, so a
/// hash-consed expression whose tree expansion is exponential lowers in time
/// proportional to its graph.  And the blaster keeps its encoding between
/// calls, so an incremental caller asking a series of related questions
/// encodes each subterm once.
///
/// Bits are ordered least significant first throughout, which is the order the
/// carries run in and therefore the order that keeps every loop in the
/// implementation forward.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SOLVER_BITBLASTER_H
#define NEVERD_SOLVER_BITBLASTER_H

#include "neverd/solver/CnfEncoder.h"
#include "neverd/solver/SatTypes.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd::solver {

/// The bits of one value, least significant first.
using BitLits = llvm::SmallVector<SatLit, 16>;

/// Bounds on what one blaster will encode.
///
/// Neither bound is a correctness condition — the encoding is exact at any
/// width — they exist so that a caller with a deadline gets an answer of
/// "not within your budget" instead of a process that stops responding.
struct BlastLimits {
  /// Widest value that will be encoded.  A multiplication at width \c w costs
  /// on the order of `w^2` adder cells, so the default admits a 256-bit word
  /// while still refusing a width that could only have come from a bug.
  uint32_t MaxWidth = 256;

  /// Gates one blaster may create before it gives up.
  size_t MaxGates = size_t(1) << 22;
};

/// Why an encoding stopped.
enum class BlastError : uint8_t {
  None,
  /// A node was wider than \c BlastLimits::MaxWidth.
  WidthTooLarge,
  /// The gate budget ran out.
  TooManyGates,
  /// The expression was not shaped the way its operator requires — a
  /// predicate asked of a value that is not one bit wide, say.  This is a
  /// defect in the caller rather than a limit being reached.
  Malformed,
};

const char *blastErrorName(BlastError E);

/// Encodes expressions from one \c SymContext into one \c CnfEncoder.
///
/// The blaster borrows both and holds no state that outlives them.  Its own
/// state is a cache from expression node to the literals standing for that
/// node's bits, which is what makes repeated and incremental use cheap.  A
/// context only ever grows, so the cache never needs invalidating.
///
/// Encoding walks the expression graph in an order where every operand
/// precedes its user, iteratively.  Obfuscated input is routinely thousands of
/// nodes deep, so recursion here would be a crash rather than a slowdown.
class BitBlaster {
public:
  BitBlaster(const symbolic::SymContext &Ctx, CnfEncoder &Enc,
             const BlastLimits &Limits = BlastLimits());

  /// Encode \p R and copy the literals standing for its bits into \p Out,
  /// least significant first.  Returns false when a limit was reached, leaving
  /// the reason in \c error(); the blaster is then unusable.
  bool blast(symbolic::SymRef R, BitLits &Out);

  /// Encode a one-bit expression and hand back the single literal that is true
  /// exactly when it holds.
  bool blastPredicate(symbolic::SymRef R, SatLit &Out);

  /// The literals standing for the bits of variable \p VarId, or an empty
  /// range if no expression encoded so far mentions it.
  ///
  /// Valid until the next call to \c blast, which may reallocate the pool the
  /// range points into.  A caller that keeps the bits across encodings copies
  /// them.
  llvm::ArrayRef<SatLit> variableBits(uint32_t VarId) const;

  /// Variable ids encoded so far, in the order they were first reached.  A
  /// model extractor walks these to decide which variables it can report.
  llvm::ArrayRef<uint32_t> encodedVars() const { return EncodedVars; }

  BlastError error() const { return Error; }
  bool ok() const { return Error == BlastError::None; }

  const symbolic::SymContext &context() const { return Ctx; }
  CnfEncoder &encoder() { return Enc; }

private:
  /// Where one node's bits live in \c BitPool.
  struct Slice {
    uint32_t First = 0;
    uint32_t Width = 0;
  };

  bool encodeReachable(symbolic::SymRef Root);
  bool encodeNode(symbolic::SymRef R);

  /// Record \p Bits as the encoding of \p R.
  ///
  /// An operator builds its result in a local buffer and stores it here rather
  /// than writing into the pool as it goes, because appending can move the
  /// pool and the operator is reading its operands out of it at the time.
  void store(symbolic::SymRef R, llvm::ArrayRef<SatLit> Bits);

  llvm::ArrayRef<SatLit> bitsOf(symbolic::SymRef R) const {
    const Slice &S = Encoded[R.index()];
    return llvm::ArrayRef<SatLit>(BitPool.data() + S.First, S.Width);
  }
  bool isEncoded(symbolic::SymRef R) const {
    return R.index() < Encoded.size() && Encoded[R.index()].Width != 0;
  }

  /// True while the encoder has built fewer gates than the budget allows.
  /// Checked once per node rather than once per gate, so a single very wide
  /// operator may overshoot by its own cost before the budget is noticed.
  bool withinGateBudget() const;
  bool fail(BlastError E);

  const symbolic::SymContext &Ctx;
  CnfEncoder &Enc;
  BlastLimits Limits;
  BlastError Error = BlastError::None;

  /// Gates the encoder had built when this blaster started, so that the budget
  /// measures this blaster's own work rather than the encoder's history.
  size_t GateBase = 0;

  std::vector<SatLit> BitPool;
  /// Indexed by expression node index; a zero width means "not yet encoded".
  std::vector<Slice> Encoded;
  /// Indexed by variable id, into \c BitPool.
  std::vector<Slice> VarSlices;
  std::vector<uint32_t> EncodedVars;

  /// Scratch reused by the traversal so that a large expression does not
  /// allocate once per node.  Nodes reached by the current traversal carry
  /// \c Visit in \c Stamp, which avoids clearing a per-node array on every
  /// call — incremental use makes many small calls against one large context.
  std::vector<uint32_t> Stamp;
  uint32_t Visit = 0;
  std::vector<uint32_t> Order;
  std::vector<uint32_t> Work;
};

} // namespace neverd::solver

#endif // NEVERD_SOLVER_BITBLASTER_H
