//===- FunctionDelta.h - Blocks and instructions of a matched pair -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What changed inside two corresponding functions: which basic blocks
/// correspond, which control-flow edges both have, and how their
/// instructions line up.  Blocks pair like functions do, by features unique
/// among the blocks still unpaired, then along matched edges; instructions
/// of paired blocks align so that identical normalized instructions line
/// up, then instructions differing only in a constant, then instructions
/// with the same mnemonic. Every completed block alignment maximizes the
/// shared weights (4/3/2 respectively); large ranges use linear frontiers and
/// bounded dense leaves instead of reducing alignment quality. This remains
/// structural evidence, and repeated instructions can have multiple optima.
///
/// The delta is evidence about the binaries, not about their source: an
/// instruction that differs only in an address it refers to has moved, and
/// a changed constant or condition stays visible even when the flow graph
/// and the function's size are unchanged.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FUNCTIONDELTA_H
#define NEVERD_DIFF_FUNCTIONDELTA_H

#include "neverd/diff/Comparison.h"
#include "neverd/diff/Program.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::diff {

/// How two aligned instructions compare.
enum class RowKind : uint8_t {
  /// The same bytes.
  Identical,
  /// The same normalized instruction at moved addresses.
  Moved,
  /// The same instruction but for a scalar constant.
  Constant,
  /// The same mnemonic with other operands.
  Operand,
  /// Another instruction in the same place.
  Replaced,
  /// Only in the secondary function.
  Inserted,
  /// Only in the primary function.
  Deleted,
};
llvm::StringRef rowKindName(RowKind Kind);
std::optional<RowKind> rowKindNamed(llvm::StringRef Name);

/// One line of the aligned listing: an instruction of either side or both,
/// by index into each program's instructions, and the blocks they belong
/// to relative to each function's first block.
struct DeltaRow {
  uint32_t Primary = NoIndex;
  uint32_t Secondary = NoIndex;
  uint32_t PrimaryBlock = NoIndex;
  uint32_t SecondaryBlock = NoIndex;
  RowKind Kind = RowKind::Identical;
};

/// Two corresponding blocks, relative to each function's first block.
struct BlockPair {
  uint32_t Primary = NoIndex;
  uint32_t Secondary = NoIndex;
  BlockStep Step = BlockStep::BlockBody;
  /// Whether every aligned instruction is identical or moved.
  bool Unchanged = false;
};

/// How an edge of one side fares in the other.
enum class EdgeState : uint8_t {
  /// The other side has the edge between the corresponding blocks.
  Matched,
  /// The other side has it with the branch condition inverted.
  Inverted,
  /// The other side lacks it.
  Unmatched,
};

/// The delta of two corresponding functions.
struct FunctionDelta {
  ComparisonState State = ComparisonState::Complete;
  /// False when cancellation/deadline stopped measurement; never unchanged.
  bool ComputationComplete = true;
  bool AnalysisComplete = true;
  /// No code body is available on either side. Similarity is not applicable.
  bool IdentityOnly = false;
  /// Paired blocks, in primary block order.
  std::vector<BlockPair> Blocks;
  /// Each block's index into Blocks, or NoIndex.
  std::vector<uint32_t> PrimaryBlockPair;
  std::vector<uint32_t> SecondaryBlockPair;
  /// Each edge of either side, in edge order.
  std::vector<EdgeState> PrimaryEdges;
  std::vector<EdgeState> SecondaryEdges;
  /// The aligned listing, in primary address order with secondary-only
  /// blocks placed after the primary block before them.
  std::vector<DeltaRow> Rows;
  /// ChangeKind mask.
  uint32_t Changes = 0;
  double Similarity = 0;
  uint32_t MatchedEdges = 0;
  /// Rows whose instructions have identical normalized forms.
  uint32_t EqualInstructions = 0;
  uint32_t PrimaryLoops = 0;
  uint32_t SecondaryLoops = 0;
};

/// Which secondary function corresponds to each primary function, for
/// deciding whether two calls reach corresponding functions.
struct Correspondence {
  llvm::ArrayRef<uint32_t> PrimaryToSecondary;
  llvm::ArrayRef<uint32_t> SecondaryToPrimary;
};

/// The delta of primary function \p PF and secondary function \p SF.
FunctionDelta compareFunctions(const Program &Primary, uint32_t PF,
                               const Program &Secondary, uint32_t SF,
                               const Correspondence &Map,
                               const ComparisonControl &Control = {});

/// Number of loops (back edges of a depth-first walk from the entry) in
/// \p F.
uint32_t loopCount(const Program &P, const Function &F);

} // namespace neverd::diff

#endif // NEVERD_DIFF_FUNCTIONDELTA_H
