//===- SEHFilterValues.h - SEH filters as __except expressions -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A Windows SEH filter decides during the exception dispatch, before anything
/// unwinds, whether its `__except` arm runs.  x86 keeps a filter as code of
/// its function that the runtime enters in the function's frame; x64, ARM and
/// AArch64 as a separate callback that reaches the parent's frame through its
/// EstablisherFrame argument or the parent's callee-saved registers.  Either
/// way C says it as the expression of `__except (...)`.  This folds a filter's
/// statements, already spelled in the frame of the function that holds the
/// `__try`, into that one expression (HighEHClause::FilterValue).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_STRUCTURE_SEHFILTERVALUES_H
#define NEVERD_IR_HIGH_STRUCTURE_SEHFILTERVALUES_H

#include "neverd/ir/high/HighIR.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd::seh_filter {

/// The <excpt.h> intrinsics a filter's value reads.
inline constexpr llvm::StringLiteral kExceptionInformation =
    "GetExceptionInformation";
inline constexpr llvm::StringLiteral kExceptionCode = "GetExceptionCode";

/// Past what only changes how \p E is read as an integer.
const HighExpr *peelViews(const HighExpr *E);

/// Whether \p E is a call of the function named \p Name.
bool callsNamed(const HighExpr *E, llvm::StringRef Name);

/// A call of the <excpt.h> intrinsic \p Name, of type \p Type;
/// GetExceptionInformation() is a pointer whatever reads it.
ExprPtr callOfRuntime(llvm::StringRef Name, TypeRef Type);

/// \p E once a read under it became a call of another width, as a pointer the
/// machine held in a word: only a conversion can say that.
ExprPtr keepViewValid(ExprPtr E);

/// Whether \p E names the stack pointer the function was entered with, as what
/// the function's frame accesses are written from.
bool isEntryStackPointer(const HighExpr &E, Arch TargetArch);

/// The distance of \p E from the function's entry stack pointer, directly or
/// through a name \p Frames gives a fixed distance.
std::optional<int64_t> frameDistance(const HighExpr *E,
                                     const std::map<VarKey, int64_t> &Frames,
                                     Arch TargetArch);

/// Reads of the exception pointers: the frame word \p Slot that holds them, or
/// a GetExceptionInformation() call, is GetExceptionInformation(), and the
/// 32-bit read two loads past it GetExceptionCode().
class ExceptionReads {
public:
  ExceptionReads(const std::map<VarKey, int64_t> &Frames,
                 std::optional<int64_t> Slot, Arch TargetArch,
                 const std::set<int64_t> *CodeSlots = nullptr)
      : Frames(Frames), Slot(Slot), TargetArch(TargetArch),
        CodeSlots(CodeSlots) {}

  /// How many loads \p E is past the exception pointers' slot, with the first
  /// load being one: the record's pointer is two.
  unsigned depthOf(const HighExpr *E) const;

  void noteAssign(const HighExpr &Dst, const HighExpr &Value);

  /// \p E with the reads it holds named.
  ExprPtr rewrite(const ExprPtr &E, unsigned Budget) const;
  ExprPtr rewrite(const ExprPtr &E) const;

private:
  const std::map<VarKey, int64_t> &Frames;
  std::optional<int64_t> Slot;
  Arch TargetArch;
  const std::set<int64_t> *CodeSlots;
  std::map<VarKey, unsigned> Depths;
};

/// How a filter's statements see the frame it decides in.
struct FilterFrame {
  Arch TargetArch = Arch::Unknown;
  /// The names the frame pointer goes by where the runtime enters the filter,
  /// by distance from the entry stack pointer.  Null for none.
  const std::map<VarKey, int64_t> *Frames = nullptr;
  /// The frame word that holds the exception pointers, when the filter reads
  /// them there rather than as GetExceptionInformation().
  std::optional<int64_t> PointersSlot;
  /// Whether a write to the frame word at a distance from the entry stack
  /// pointer is the filter's own business: it says whether the store can go,
  /// and takes the value in case that makes it GetExceptionCode()'s.  Unset,
  /// every write stays.
  std::function<bool(int64_t, const HighExpr &)> IsPrivate;
  /// Where the frame's own stack ends: a write below it is a call's argument.
  std::optional<int64_t> StackFloor;
};

/// One expression for a filter body: its assignments folded into what reads
/// them, what it does before it decides as the left side of a comma
/// (`BinOp NOP`), and its branches as conditional expressions (`BinOp SELECT`).
/// Null when one expression cannot say what the body does in its order.
ExprPtr foldFilterBody(const std::vector<HighStmt> &Body,
                       const FilterFrame &Frame);

} // namespace neverd::seh_filter

#endif // NEVERD_IR_HIGH_STRUCTURE_SEHFILTERVALUES_H
