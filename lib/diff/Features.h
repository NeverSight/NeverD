//===- Features.h - What identifies a function or block ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The features the comparison pairs functions and blocks by, computed once
/// per program.  Each exact feature is a 64-bit key, zero when the function
/// is not eligible for it (too small, or without what the feature counts);
/// the sketch estimates how many instruction runs two bodies share.
///
/// NeverD's flow-graph MD index follows Dullien and Rolles ("Graph-based
/// comparison of executable objects", SSTIC 2005): every edge is described
/// by its source's level, the source's in- and out-degree and the target's
/// in- and out-degree.  Where the paper sums a real-valued function of each
/// tuple, NeverD sums a 64-bit hash of it, so the index is exact, does not
/// depend on the order edges are visited or on floating-point evaluation,
/// and still does not change when blocks are reordered.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_DIFF_FEATURES_H
#define NEVERD_LIB_DIFF_FEATURES_H

#include "neverd/diff/Comparison.h"
#include "neverd/diff/Program.h"

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <cstdint>
#include <vector>

namespace neverd::diff {

/// Slots of a function's MinHash sketch.
inline constexpr unsigned SketchSize = 32;
/// The sketch's locality-sensitive buckets: SketchBands bands of
/// SketchSize / SketchBands slots each.
inline constexpr unsigned SketchBands = 8;

struct FunctionFeatures {
  /// A name the program states; zero for a generated name.
  uint64_t Name = 0;
  /// The imported symbol, for an import.
  uint64_t Import = 0;
  uint64_t Bytes = 0;
  uint64_t Shape = 0;
  /// Complete address-independent instruction stream without NOP padding or
  /// an entry ENDBR marker. Only a candidate key: bytes, CFG, scalar operands
  /// and referenced identities are still measured before acceptance.
  uint64_t BodyWithoutPadding = 0;
  /// The same stream including padding, to require an actual layout edit
  /// before the additional candidate key broadens the existing search.
  uint64_t BodyWithPadding = 0;
  /// Address-independent instructions, block boundaries and exact CFG. Used
  /// to align reference occurrences, never to assert normalized equality.
  uint64_t ReferenceLayout = 0;
  /// Complete call-free conditional entry without padding/landing markers.
  /// Proposes candidates across CFG edits; it never establishes identity.
  uint64_t EntryBody = 0;
  uint64_t Mnemonics = 0;
  uint64_t Prime = 0;
  uint64_t GraphTopDown = 0;
  uint64_t GraphBottomUp = 0;
  uint64_t Strings = 0;
  uint64_t Constants = 0;
  uint64_t ImportCalls = 0;
  uint64_t Neighborhood = 0;
  uint64_t CallGraph = 0;
  uint32_t Instructions = 0;
  uint32_t Blocks = 0;
  uint32_t Edges = 0;
  uint32_t Loops = 0;
  uint32_t Calls = 0;
  /// A single-block, straight-line prelude followed by an admitted tail jump.
  /// Thunks, recursive jumps and other control transfers are not eligible.
  uint32_t TailCallee = NoIndex;
  struct AddressSite {
    uint32_t Target;
    uint32_t Instruction;
    uint32_t Operand;
  };
  /// Instruction/operand-bound address-taking uses and modeled transfers.
  /// NoIndex as operand denotes a transfer, separate from explicit operands.
  std::vector<AddressSite> AddressSites;
  struct DataSite {
    uint64_t Key;
    uint32_t Instruction;
    uint32_t Operand;
    ReferenceKind Kind;
  };
  /// Non-function address operands, retaining their kind and occurrence.
  std::vector<DataSite> DataSites;
  /// Every unresolved data reference has an instruction/operand occurrence.
  bool DataSitesComplete = true;
  uint32_t FunctionReferences = 0;
  /// Raw address encodings vary with layout and cannot identify duplicate
  /// normalized bodies, even when their referenced objects correspond.
  bool AddressEncodings = false;
  bool UnresolvedData = false;
  bool UnresolvedCallee = false;
  /// Whether the sketch holds anything.
  bool Sketched = false;
  std::array<uint32_t, SketchSize> Sketch{};
};

/// A compressed adjacency list: the neighbors of node I are
/// Targets[Start[I] .. Start[I + 1]).
struct Adjacency {
  std::vector<uint32_t> Start;
  std::vector<uint32_t> Targets;

  llvm::ArrayRef<uint32_t> of(uint32_t I) const {
    return llvm::ArrayRef(Targets).slice(Start[I], Start[I + 1] - Start[I]);
  }
};

struct ProgramFeatures {
  std::vector<FunctionFeatures> Functions;
  /// Distinct functions each function calls, and that call it, ascending.
  Adjacency Callees;
  Adjacency Callers;
  /// Address-taking references, kept separate from modeled call edges.
  Adjacency Referents;
  Adjacency Referrers;
  /// Every call in instruction order; NoIndex retains an unresolved call.
  Adjacency CallSites;
  /// Eligible straight-line tail callers, indexed by their direct callee.
  Adjacency TailCallers;
  /// What each function's thunks lead to: the function itself, or for a
  /// thunk the function it passes control to.
  std::vector<uint32_t> Resolved;
  /// hashText of each mnemonic, by Instruction::Mnemonic.
  std::vector<uint64_t> MnemonicKeys;
};

/// Computes the features of every function of \p P, functions in parallel.
ProgramFeatures computeFeatures(const Program &P,
                                const ComparisonControl &Control = {});

/// The share of sketch slots two sketched functions agree on.
double sketchSimilarity(const FunctionFeatures &A, const FunctionFeatures &B);

/// The key of sketch band \p Band.
uint64_t sketchBand(const FunctionFeatures &F, unsigned Band);

/// Per-block levels and degrees of one function's flow graph.
struct GraphShape {
  std::vector<uint32_t> TopLevel;
  std::vector<uint32_t> BottomLevel;
  std::vector<uint32_t> In;
  std::vector<uint32_t> Out;
  uint32_t Loops = 0;
};
GraphShape graphShape(const Program &P, const Function &F,
                      const ComparisonControl &Control = {});

/// The MD index of one edge tuple.
uint64_t edgeIndex(uint32_t Level, uint32_t SourceIn, uint32_t SourceOut,
                   uint32_t TargetIn, uint32_t TargetOut);

} // namespace neverd::diff

#endif // NEVERD_LIB_DIFF_FEATURES_H
