//===- BitBlastDetail.h - Circuits over literal vectors ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The arithmetic circuits the bit-blaster is assembled from, expressed over
/// plain vectors of literals rather than over expressions.
///
/// Keeping them separate from the expression walk is what makes each one
/// readable on its own: an adder here is a chain of full adders and nothing
/// else, with no operand lookup, no width negotiation and no cache.  It also
/// means the circuits can be exercised directly, which is how the tests pin
/// down the semantics of a divider without going through a solver query.
///
/// Every vector is least significant bit first, and every operation is modulo
/// two to the width, matching the expression language exactly.  Operands of a
/// binary circuit have equal length unless a comment says otherwise, and no
/// output may alias an input.
///
/// This header is an implementation detail of the solver library and should
/// not be included outside lib/solver/bv/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SOLVER_BV_BITBLASTDETAIL_H
#define NEVERD_SOLVER_BV_BITBLASTDETAIL_H

#include "neverd/solver/CnfEncoder.h"
#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::solver::detail {

using LitSpan = llvm::ArrayRef<SatLit>;
using LitVec = llvm::SmallVector<SatLit, 16>;

//===----------------------------------------------------------------------===//
// Selection — BitBlastArith.cpp
//===----------------------------------------------------------------------===//

/// `Cond ? Then : Else`, bit by bit.
void selectBits(CnfEncoder &E, SatLit Cond, LitSpan Then, LitSpan Else,
                LitVec &Out);

//===----------------------------------------------------------------------===//
// Arithmetic — BitBlastArith.cpp
//===----------------------------------------------------------------------===//

/// `A + B + CarryIn` modulo two to the width.  \p CarryOut, when given,
/// receives the carry leaving the top bit; when it is not asked for, the top
/// carry cell is not built at all.
void addBits(CnfEncoder &E, LitSpan A, LitSpan B, SatLit CarryIn, LitVec &Out,
             SatLit *CarryOut = nullptr);

/// `-A`, which is the complement plus one.
void negateBits(CnfEncoder &E, LitSpan A, LitVec &Out);

/// `A * B` modulo two to the width.
///
/// Between two unknown values this is the sum of the shifted rows of the long
/// multiplication, which costs on the order of a squared width in adder cells.
///
/// A product by a literal — which is most of what recovered code contains, and
/// all of what a subtraction is, since the expression language spells one as a
/// multiplication by minus one — is instead recoded into shifted copies of the
/// other operand to add and to subtract.  A run of set bits in the literal
/// becomes one addition and one subtraction however long the run is, so the
/// cost follows the shape of the literal rather than its population count, and
/// minus one costs a single negation.
void multiplyBits(CnfEncoder &E, LitSpan A, LitSpan B, LitVec &Out);

/// Unsigned quotient and remainder of `A` by `B`, either output optional.
///
/// This is long division: one step per bit, each shifting a bit of the
/// dividend into a running remainder and subtracting the divisor when it fits.
/// Division by zero needs no special case, and that is not a coincidence.  The
/// expression language totalises it the way the circuit already behaves — the
/// subtraction always fits, so every quotient bit comes out set and the
/// remainder is left holding the dividend.
void divideBits(CnfEncoder &E, LitSpan A, LitSpan B, LitVec *Quotient,
                LitVec *Remainder);

/// Signed quotient and remainder, truncating towards zero.
///
/// Magnitudes are divided and the signs applied afterwards: the quotient takes
/// the sign of the operands combined, the remainder the sign of the dividend.
/// The awkward cases fall out rather than being tested for — dividing by zero,
/// and dividing the most negative value by minus one, both come back with the
/// values the expression language defines.
void divideSignedBits(CnfEncoder &E, LitSpan A, LitSpan B, LitVec *Quotient,
                      LitVec *Remainder);

//===----------------------------------------------------------------------===//
// Comparison — BitBlastArith.cpp
//===----------------------------------------------------------------------===//

/// True when every bit agrees.
SatLit equalBits(CnfEncoder &E, LitSpan A, LitSpan B);

/// Unsigned `A >= B`, as the carry out of `A - B`.
///
/// Comparing through a subtraction rather than by walking down from the top
/// bit costs one majority gate per bit and nothing else, because the sums are
/// never built — only the carry chain is.
SatLit unsignedAtLeast(CnfEncoder &E, LitSpan A, LitSpan B);

/// `A < B` or `A <= B`, signed or unsigned.  A signed comparison is the
/// unsigned one with both sign bits complemented, because complementing the
/// sign bit is exactly the order-preserving map between the two readings.
SatLit lessThanBits(CnfEncoder &E, LitSpan A, LitSpan B, bool OrEqual,
                    bool Signed);

//===----------------------------------------------------------------------===//
// Shifts and rotates — BitBlastShift.cpp
//===----------------------------------------------------------------------===//

enum class ShiftKind : uint8_t {
  Left,
  LogicalRight,
  ArithmeticRight,
  RotateLeft,
  RotateRight,
};

/// Shift or rotate \p A by a fixed amount, which is pure rewiring and costs no
/// gates at all.  For the shifts the amount is clamped to the width, so a
/// shift past the end produces the fill value.
void shiftBitsByConstant(CnfEncoder &E, LitSpan A, uint64_t Amount,
                         ShiftKind Kind, LitVec &Out);

/// Shift or rotate \p A by a value that is not known.
///
/// One stage per bit of the amount, each either applying a fixed power-of-two
/// shift or passing its input through, which is a ladder of multiplexers whose
/// depth is logarithmic in the width rather than linear.  Shift amounts at or
/// beyond the width produce the fill value; rotate amounts are taken modulo
/// the width, exactly as the expression language defines them.
void shiftBits(CnfEncoder &E, LitSpan A, LitSpan Amount, ShiftKind Kind,
               LitVec &Out);

/// The value of \p Amount when every one of its bits is already decided,
/// saturated at \p Limit.  Saturating is enough for a shift, where every
/// amount at or beyond the width has the same effect, and it keeps the result
/// in a machine word however wide the amount was.
std::optional<uint64_t> constantAmount(const CnfEncoder &E, LitSpan Amount,
                                       uint64_t Limit);

/// The value of \p Amount modulo \p Modulus when every one of its bits is
/// already decided.
///
/// A rotation needs the residue rather than a saturated amount, and the
/// residue is accumulated bit by bit so that an amount far wider than a
/// machine word still reduces exactly.
std::optional<uint64_t> constantResidue(const CnfEncoder &E, LitSpan Amount,
                                        uint64_t Modulus);

} // namespace neverd::solver::detail

#endif // NEVERD_SOLVER_BV_BITBLASTDETAIL_H
