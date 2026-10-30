//===- Program.h - One side of a binary comparison --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What the binary comparison reads of one analyzed program: its functions,
/// their basic blocks and control-flow edges, and each instruction's bytes,
/// mnemonic and normalized operands.  The model holds no loader or session
/// state; the SDK fills it from NeverD's analysis, and tests build it by
/// hand.  Addresses are the program's own, so a rebased program keeps its
/// identities relative to its image base.
///
/// An instruction is described at three levels.  Its bytes are what the
/// file holds.  Its shape names the mnemonic and each operand, with every
/// address replaced by what it refers to (a function, a string, an import,
/// a data object or a place in the same function), so code that only moved
/// keeps its shape.  Its skeleton is the shape without scalar immediates and
/// displacements, so two instructions with equal skeletons and different
/// shapes differ in a constant.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_PROGRAM_H
#define NEVERD_DIFF_PROGRAM_H

#include "neverd/diff/Control.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::diff {

struct DataFlowEvidence;

/// An index that refers to nothing.
inline constexpr uint32_t NoIndex = std::numeric_limits<uint32_t>::max();

/// How an instruction passes control on.
enum class InstructionFlow : uint8_t {
  /// To the next instruction.
  None,
  /// A direct call; Instruction::Callee names the function when known.
  Call,
  IndirectCall,
  /// An unconditional direct jump.
  Jump,
  /// A conditional direct jump.
  Branch,
  IndirectJump,
  Return,
  /// The analysis could not model the instruction.
  Unknown,
};

/// What a reference refers to.
enum class ReferenceKind : uint8_t {
  /// A string; the key hashes its text.
  String,
  /// A scalar immediate; the key is its value.
  Constant,
  /// A named data object or import; the key hashes its name.
  Data,
  /// A known native function address. The key is its entry address, not a
  /// call edge. A filtered snapshot may omit that function's body.
  Function,
  /// An address whose object identity is unknown. Its key is scoped to the
  /// exact input/analysis identity; equal RVAs in different inputs are not
  /// evidence that the referenced objects correspond.
  UnresolvedData,
};

/// One address-taking operand. Occurrences retain position and multiplicity;
/// a function-wide target set cannot distinguish swapped callback arguments.
struct AddressReference {
  uint64_t Key = 0;
  uint32_t Operand = 0;
  ReferenceKind Kind = ReferenceKind::Function;
};

/// One decoded instruction.
struct Instruction {
  uint64_t Address = 0;
  /// Original decoded operands for inspection; never used as identity.
  std::string Operands;
  /// Original direct transfer or import-slot target, when known.
  std::optional<uint64_t> Target;
  /// The normalized instruction: mnemonic and operands, each address
  /// replaced by what it refers to.
  uint64_t Shape = 0;
  /// The shape without scalar immediates and displacements.
  uint64_t Skeleton = 0;
  /// Shape retaining scalar operands but replacing address identities with a
  /// category marker. Distinguishes reference gaps from observed operand
  /// changes and proposes structural candidates; it never establishes
  /// normalized equality.
  uint64_t AddressIndependentShape = 0;
  /// Where the instruction's bytes begin in Program::Code.
  uint32_t CodeOffset = 0;
  /// The modeled call or tail transfer's target, including a resolved import
  /// slot, as an index into Program::Functions, or NoIndex. Consumers use this
  /// admitted relation rather than inferring callees again from opcode kind.
  uint32_t Callee = NoIndex;
  /// A modeled transfer whose target was omitted from this snapshot.
  bool UnresolvedCallee = false;
  bool HasUnresolvedData = false;
  uint32_t FirstAddressReference = 0;
  uint32_t AddressReferenceCount = 0;
  /// Index into Program::Mnemonics.
  uint16_t Mnemonic = 0;
  uint8_t Size = 0;
  InstructionFlow Flow = InstructionFlow::None;
};

/// How a control-flow edge leaves its block.
enum class EdgeKind : uint8_t {
  /// To the next block, without a jump.
  Fallthrough,
  /// A conditional branch taken.
  Taken,
  /// A conditional branch not taken.
  NotTaken,
  /// An unconditional jump.
  Jump,
  /// One target of a jump table.
  Switch,
  /// To an exception handler.
  Exception,
};

/// A control-flow edge between two blocks of one function, as indices
/// relative to the function's first block.
struct Edge {
  uint32_t From = 0;
  uint32_t To = 0;
  EdgeKind Kind = EdgeKind::Jump;
};

/// A basic block: a run of instructions control enters only at the first.
struct Block {
  uint64_t Address = 0;
  /// Index of the first instruction in Program::Instructions.
  uint32_t FirstInstruction = 0;
  uint32_t InstructionCount = 0;
};

/// What a function is besides code of the program.
enum FunctionTrait : uint16_t {
  /// An imported symbol with no body in this program.
  TraitImport = 1u << 0,
  /// A stub that only passes control to another function or an import.
  TraitThunk = 1u << 1,
  /// Recognized library or runtime code.
  TraitLibrary = 1u << 2,
  /// Exported by the program.
  TraitExported = 1u << 3,
  /// The program's entry point.
  TraitEntryPoint = 1u << 4,
  /// The analysis could not describe the whole body: an instruction it
  /// does not model, or a jump it could not follow.
  TraitIncomplete = 1u << 5,
};

/// Where a function's name comes from.  Every name but a generated one may
/// pair functions by name.
enum class NameSource : uint8_t {
  /// Made up by the analysis from the address.
  Generated,
  /// Given by a library signature.
  Signature,
  /// A symbol table entry.
  Symbol,
  /// Debug information.
  Debug,
  /// An export.
  Export,
  /// An import.
  Import,
};

/// One function.  Blocks, edges and references are runs of the program's
/// arrays.
struct Function {
  uint64_t Address = 0;
  /// Bytes from the entry the function's code covers; zero when unknown.
  uint64_t Size = 0;
  std::string Name;
  /// The library an import comes from, when stated.
  std::string Module;
  NameSource Source = NameSource::Generated;
  uint16_t Traits = 0;
  uint32_t FirstBlock = 0;
  uint32_t BlockCount = 0;
  uint32_t FirstEdge = 0;
  uint32_t EdgeCount = 0;
  uint32_t FirstReference = 0;
  uint32_t ReferenceCount = 0;
  /// The entry block, relative to FirstBlock; NoIndex for a function
  /// without blocks.
  uint32_t EntryBlock = NoIndex;

  bool has(FunctionTrait Trait) const { return (Traits & Trait) != 0; }
  /// Whether the function has code of this program to compare.
  bool hasBody() const { return BlockCount != 0; }
};

/// One distinct thing a function's instructions refer to.
struct Reference {
  ReferenceKind Kind = ReferenceKind::Constant;
  uint64_t Key = 0;
  /// The block that first refers to it, relative to the function's first
  /// block.
  uint32_t Block = 0;
};

/// What identifies the analyzed input a program came from.
struct ProgramIdentity {
  /// The input file as the caller named it.
  std::string Path;
  /// SHA-256 of the input file, in lowercase hex; empty when unknown.
  std::string SHA256;
  /// When Path names an NDDB container, SHA256 still identifies its embedded
  /// binary. This names that input and binds its saved analysis project files.
  std::string ProjectInputName, ProjectStateSHA256;
  std::string Format;
  std::string Architecture;
  std::string ABI;
  std::string Normalization;
  /// Content-bound analysis revision, independent of transient list indices.
  std::string Revision;
  unsigned Bits = 0;
  /// "little" or "big"; empty when the loader does not state it.
  std::string Endian;
  uint64_t ImageBase = 0;
  uint64_t EntryPoint = 0;
  /// The analysis settings the program was read with, as one stable
  /// string: two programs read with the same settings from the same input
  /// are the same program.
  std::string Analysis;
  /// Whether function discovery finished; a partial discovery leaves
  /// functions out.
  bool DiscoveryComplete = true;
};

inline constexpr llvm::StringLiteral NativeX86Normalization =
    "native-x86-operands-v3";
inline constexpr llvm::StringLiteral NativeAArch64Normalization =
    "native-aarch64-operands-v3";
/// All revisions preserve scalar operands; v3 distinguishes reference gaps.
bool isNativeX86Normalization(llvm::StringRef Profile);

/// Versioned, length-delimited identity of an analyzed snapshot. Preserves
/// exact field bytes and excludes only the caller's path. This is an encoding,
/// not a digest or authentication of the input or its analysis.
std::string programAnalysisIdentity(const ProgramIdentity &Identity);

/// One side of a comparison.
struct Program {
  ProgramIdentity Identity;
  /// Optional, bounded SSA observations. Never borrows the input session.
  std::shared_ptr<const DataFlowEvidence> DataFlow;
  /// In ascending address order, one per address.
  std::vector<Function> Functions;
  std::vector<Block> Blocks;
  std::vector<Edge> Edges;
  std::vector<Instruction> Instructions;
  std::vector<Reference> References;
  std::vector<AddressReference> AddressReferences;
  /// The bytes of every instruction, at Instruction::CodeOffset.
  std::vector<uint8_t> Code;
  /// Distinct mnemonics, by Instruction::Mnemonic.
  std::vector<std::string> Mnemonics;
  /// What the reading left out, for the comparison's coverage.
  std::vector<std::string> Warnings;

  llvm::ArrayRef<Block> blocks(const Function &F) const {
    return llvm::ArrayRef(Blocks).slice(F.FirstBlock, F.BlockCount);
  }
  llvm::ArrayRef<Edge> edges(const Function &F) const {
    return llvm::ArrayRef(Edges).slice(F.FirstEdge, F.EdgeCount);
  }
  llvm::ArrayRef<Reference> references(const Function &F) const {
    return llvm::ArrayRef(References).slice(F.FirstReference, F.ReferenceCount);
  }
  llvm::ArrayRef<Instruction> instructions(const Block &B) const {
    return llvm::ArrayRef(Instructions)
        .slice(B.FirstInstruction, B.InstructionCount);
  }
  llvm::ArrayRef<uint8_t> bytes(const Instruction &I) const {
    return llvm::ArrayRef(Code).slice(I.CodeOffset, I.Size);
  }
  llvm::ArrayRef<AddressReference>
  addressReferences(const Instruction &I) const {
    return llvm::ArrayRef(AddressReferences)
        .slice(I.FirstAddressReference, I.AddressReferenceCount);
  }
  llvm::StringRef mnemonic(const Instruction &I) const {
    return I.Mnemonic < Mnemonics.size()
               ? llvm::StringRef(Mnemonics[I.Mnemonic])
               : llvm::StringRef();
  }
  /// Number of instructions in \p F.
  uint64_t instructionCount(const Function &F) const;
  /// The function that begins at \p Address.
  std::optional<uint32_t> functionAt(uint64_t Address) const;
  /// The function whose blocks hold \p Address.
  std::optional<uint32_t> functionContaining(uint64_t Address) const;

  /// Checks every index and order the comparison relies on.  An error names
  /// the first violation.
  std::optional<std::string>
  validate(const ComparisonControl &Control = {}) const;
};

/// Builds a program one function at a time, keeping its arrays consistent.
class ProgramBuilder {
public:
  explicit ProgramBuilder(ProgramIdentity Identity);

  /// Starts a function; blocks, edges and references added next are its
  /// own.  Functions must be added in ascending address order.
  void beginFunction(uint64_t Address, uint64_t Size, std::string Name,
                     NameSource Source, uint16_t Traits,
                     std::string Module = {});
  /// Adds a block of the current function and returns its index relative
  /// to the function.
  uint32_t addBlock(uint64_t Address);
  /// Adds an instruction to the last block.  \p CallTarget is the address
  /// a direct call reaches; finish() names the function that begins there.
  void
  addInstruction(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes,
                 llvm::StringRef Mnemonic, uint64_t Shape, uint64_t Skeleton,
                 InstructionFlow Flow,
                 std::optional<uint64_t> CallTarget = std::nullopt,
                 llvm::StringRef Operands = {},
                 std::optional<uint64_t> Target = std::nullopt,
                 llvm::ArrayRef<AddressReference> AddressReferences = {},
                 std::optional<uint64_t> AddressIndependentShape = std::nullopt,
                 bool HasUnresolvedData = false);
  void addEdge(uint32_t From, uint32_t To, EdgeKind Kind);
  /// Records a reference of the current function's block \p Block; a
  /// repeated reference is kept once.
  void addReference(ReferenceKind Kind, uint64_t Key, uint32_t Block);
  void setEntryBlock(uint32_t Block);
  void addTraits(uint16_t Traits);
  void warn(std::string Warning);

  /// Ends the program: every direct call whose target begins one of its
  /// functions names that function.
  Program finish();

  /// Index the next instruction will take.
  uint32_t nextInstruction() const {
    return static_cast<uint32_t>(P.Instructions.size());
  }

private:
  Program P;
  llvm::StringMap<uint16_t> MnemonicIds;
  /// Instruction index and target of each direct call.
  std::vector<std::pair<uint32_t, uint64_t>> CallTargets;
  bool InFunction = false;
  void endFunction();
};

} // namespace neverd::diff

#endif // NEVERD_DIFF_PROGRAM_H
