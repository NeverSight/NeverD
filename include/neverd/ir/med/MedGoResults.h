//===- MedGoResults.h - Go register ABI call results ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The results a call under Go's internal register ABI returns past its
/// first: which of them a function reads, and the rewrite that returns them.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDGORESULTS_H
#define NEVERD_IR_MED_MEDGORESULTS_H

#include "neverd/Common.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

/// The position of \p RegOff among Go's integer results on \p A (RAX, RBX,
/// RCX, RDI, RSI, R8-R11: goIntegerArgumentRegisters), or -1.
int goResultPosition(Arch A, uint64_t RegOff);

/// The position of \p RegOff among Go's floating results on \p A (X0-X14,
/// arm64 F0-F15), or -1.  Go numbers its integer and floating results apart,
/// each from its first register.
int goFloatResultPosition(Arch A, uint64_t RegOff);

/// The register of Go's floating result at \p Position on \p A (X0-X14,
/// arm64 V0-V15): goFloatResultPosition's inverse.
uint64_t goFloatResultRegister(Arch A, int Position);

/// The results a call returns: the position of the last, and for each the
/// bytes of its register the call's readers take, at most a word.  A
/// position no reader takes, before the last, is a whole word.
struct GoCallShape {
  int Last = -1;
  llvm::SmallVector<uint16_t, 9> Widths;
  /// The floating results: the position of the last, or -1, and the bytes
  /// of each, a float32's or a float64's.
  int LastFloat = -1;
  llvm::SmallVector<uint16_t, 16> FloatWidths;
};

/// The results \p Func reads after each call in \p Sites, before any write:
/// a read of the value the call left in a result register, in any block and
/// through the PHIs it reaches, and, for \p Returned, a value the function
/// returns.  A call that returns some results already counts them.  A read
/// that only keeps the bytes a partial write leaves, a self-zeroing idiom,
/// or a sub-register view takes no result; the view still reads as many of
/// a result's bytes as it covers.  With \p Floats, a read of a floating
/// result register counts too (GoCallShape::LastFloat).  A call none of
/// whose results past RAX is read is absent.
///
/// With \p BothBanks (and floating reads), Go's two banks of results count
/// apart, each from its first register: a call returns RAX where RAX is its
/// value and X0 (arm64 F0) where that is, and a read of the other bank's
/// first register is a result too.  A call's shape then holds no integer
/// result (Last -1) where it returns floating ones alone, and a call none of
/// whose results past those it returns is read is absent.
std::map<uint32_t, GoCallShape>
goCallResultsRead(const MedFunc &Func, Arch A, const std::set<uint32_t> &Sites,
                  llvm::ArrayRef<MedVar> Returned = {}, bool Floats = false,
                  bool BothBanks = false);

/// What a Go function's return in \p Block hands back in the register at
/// \p RegOff, as the return lowers a result field: the newest write of the
/// whole register, its PHI, the call that last changes it, or the value it
/// entered the function with (\p Value is then the copy that names it, if
/// any).  Every write of a general-purpose register defines the whole of it
/// too; a narrower value (a view copy propagation left on a temporary, the
/// entry copy of the low byte) is a value of its own, which a later write of
/// the whole register leaves stale.  A join without a PHI for the register
/// brings the same value along every edge, so any edge that reaches it will do.
/// \p ById maps the function's block ids to its blocks.  With \p Floating,
/// \p RegOff is a floating result's vector register: every write of four
/// bytes or more defines it, as a scalar's (MOVSS, MOVSD) does, but a view
/// of the register's own value does not.
struct GoReturnedValue {
  enum Kind : uint8_t { Unknown, Defined, Phi, Call, Entry } What = Unknown;
  const MedVar *Value = nullptr;
  const MedOp *Caller = nullptr;
};
GoReturnedValue
goReturnedValue(const llvm::DenseMap<int, const MedBlock *> &ById,
                const MedBlock &Block, uint64_t RegOff, Arch A,
                bool Floating = false);

/// Make each call of \p Func that \p Shapes names return Go's integer
/// results through its last position, each as wide as its shape says,
/// packed into one value laid out as the record of them (each field
/// naturally aligned), whose fields define each result register the call
/// leaves, at every width it leaves it, so the reads it reaches keep their
/// values.  A width wider than its field takes the field's value in its low
/// bytes: no reader takes more.  A call that returns some already, each a
/// word, takes the rest.  Its floating results follow its integer ones in
/// the record, each a float as wide as the shape says.  A record of floating
/// results alone (Last -1) holds no integer one: the RAX a call returned
/// before is then left as a call leaves a register it returns nothing in.
/// A call that returned X0 (arm64 F0) alone returns it as the first
/// floating result.
void defineGoCallResults(MedFunc &Func, Arch A,
                         const std::map<uint32_t, GoCallShape> &Shapes);

/// What a Go stack-ABI function does with the words of its caller's frame
/// that hold its arguments and its results (goStackFrameUse).  Offsets count
/// bytes from the first of those words.
struct GoStackFrameUse {
  /// The byte ranges it stores into, each once: an offset and a size.
  std::vector<std::pair<uint32_t, uint16_t>> Stored;
  /// The bytes some path reads before it has written them: an argument's.
  llvm::BitVector Exposed;
  struct Return {
    /// The index of the returning block.
    size_t Block = 0;
    /// The return hands back what a tail call returns, to \ref Target where
    /// the call names it.
    bool Tail = false;
    va_t Target = InvalidVA;
    /// The bytes every path to the return has written: stored, or lying at
    /// or above an address the function handed on, which a callee may have
    /// written through.
    llvm::BitVector Written;
  };
  std::vector<Return> Returns;
  /// How many bytes were followed: those the function's record gives, or
  /// limits::ir::kGoUnrecordedFrameBytes where it gives none or no size.
  uint32_t Bytes = 0;
};

/// What the stack-ABI Go function \p Func, in SSA form after its parameters
/// were recovered (detectStackParams), does with its argument words, of
/// which its record gives \p FrameBytes, if it gives any
/// (goFrameArgumentBytes).
GoStackFrameUse goStackFrameUse(const MedFunc &Func, Arch A, BinaryFormat F,
                                std::optional<uint32_t> FrameBytes);

/// The results \p Use shows: Go's stack ABI returns each result in the words
/// after the arguments, so a result is what the function stores there and no
/// path reads first.  Each is the bytes of one store within a word that every
/// return has written, the widest where stores overlap; a return for which
/// \p SharesFrame holds hands the frame to the stack-ABI routine it jumps
/// to, which returns its own.  In the order of their offsets, and none where
/// there are more than Go's result registers (goIntegerArgumentRegisters).
std::vector<MedGoStackResult> goStackResults(
    const GoStackFrameUse &Use, Arch A,
    llvm::function_ref<bool(const GoStackFrameUse::Return &)> SharesFrame);

/// Make each return in \p Blocks (indices of returning blocks) hand back the
/// function's results (MedFunc::GoStackResults) in Go's result registers,
/// the first as the value the RETURN names and a narrower one zero extended.
/// A result is the value the one store that reaches the return stored;
/// where paths store different values, a store covers it in part, or the
/// function hands the address of its word on, it is read from the word just
/// before the return.  \p Bytes are the argument bytes followed
/// (GoStackFrameUse::Bytes).  False, with nothing changed, where the
/// function has no entry stack pointer to read them from.
bool returnGoStackResults(MedFunc &Func, Arch A, BinaryFormat F, uint32_t Bytes,
                          llvm::ArrayRef<size_t> Blocks);

/// The results a call of a Go stack-ABI function returns, and whether the
/// call is a jump that hands the caller's own argument words on, which then
/// hold them already.
struct GoStackCall {
  llvm::ArrayRef<MedGoStackResult> Results;
  bool SharesFrame = false;
};

/// Make each call of \p Func that \p Sites names return its results, one as
/// the call's value and several as their record (defineGoCallResults), and
/// store each in the word of the caller's frame the callee left it in: the
/// machine's state after the call, which the caller's reads of its results
/// then find, and the next call's argument scan, where it passes a result on
/// in place.  A call whose stack pointer is not known stores none.
void storeGoStackCallResults(MedFunc &Func, Arch A,
                             const std::map<uint32_t, GoStackCall> &Sites);

/// Whether the operation at \p Index of a block's \p Ops is a store
/// storeGoStackCallResults added after a call: one at the call's address,
/// after nothing but the call's own operations, of the call's value, a
/// field of it or a view of one.
bool storesGoStackCallResult(llvm::ArrayRef<MedOp> Ops, size_t Index);

/// Whether some operation of \p Func gives the stack pointer a value that is
/// no constant away from the one the function was entered with: it loads the
/// pointer, masks it or takes another stack's.  The words a stack-ABI
/// routine stores after that are at no offset from its entry that names
/// them, its results among them.
bool goLeavesItsStack(const MedFunc &Func, Arch A, BinaryFormat F);

/// Drop the stack parameters of \p Func that lie in the words of its results
/// (MedFunc::GoStackResults), from the last down, and say how many: a result
/// is no argument.  Parameter recovery takes one for a parameter where the
/// function reads it back after storing it, and where a routine that jumps
/// on passes every word of its target's record on (detectStackParams), the
/// results' too.  A word an operation reads as the function was entered
/// stays, and ends the walk.
unsigned dropGoStackResultParameters(MedFunc &Func, Arch A);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDGORESULTS_H
