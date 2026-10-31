//===- Program.cpp - One side of a binary comparison ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Building a comparison program one function at a time, and checking the
/// indices and orders the comparison relies on.
///
//===----------------------------------------------------------------------===//

#include "neverd/diff/Program.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace neverd::diff {

bool isNativeX86Normalization(llvm::StringRef Profile) {
  return Profile == "native-x86-operands-v1" ||
         Profile == "native-x86-operands-v2" ||
         Profile == NativeX86Normalization;
}

std::string programAnalysisIdentity(const ProgramIdentity &I) {
  std::string Out = "neverd.bindiff.analysis-v1:";
  llvm::raw_string_ostream OS(Out);
  const auto Field = [&](llvm::StringRef Text) {
    OS << Text.size() << ':' << Text;
  };
  for (const auto *Text :
       {&I.SHA256, &I.ProjectInputName, &I.ProjectStateSHA256, &I.Format,
        &I.Architecture, &I.ABI, &I.Normalization, &I.Revision, &I.Endian,
        &I.Analysis})
    Field(*Text);
  Field(std::to_string(I.Bits));
  Field(std::to_string(I.ImageBase));
  Field(std::to_string(I.EntryPoint));
  Field(I.DiscoveryComplete ? "1" : "0");
  return Out;
}

uint64_t Program::instructionCount(const Function &F) const {
  uint64_t Count = 0;
  for (const Block &B : blocks(F))
    Count += B.InstructionCount;
  return Count;
}

std::optional<uint32_t> Program::functionAt(uint64_t Address) const {
  const auto It = llvm::partition_point(
      Functions, [&](const Function &F) { return F.Address < Address; });
  if (It == Functions.end() || It->Address != Address)
    return std::nullopt;
  return static_cast<uint32_t>(It - Functions.begin());
}

std::optional<uint32_t> Program::functionContaining(uint64_t Address) const {
  if (const auto Exact = functionAt(Address))
    return Exact;
  // A function's blocks may lie past later functions' entries (a cold part
  // moved out of line), so look at every block that could hold it.
  const auto Holds = [&](const Function &F) {
    for (const Block &B : blocks(F)) {
      const auto Insns = instructions(B);
      if (Insns.empty())
        continue;
      const Instruction &Last = Insns.back();
      if (Address >= B.Address && Address < Last.Address + Last.Size)
        return true;
    }
    return false;
  };
  const auto It = llvm::partition_point(
      Functions, [&](const Function &F) { return F.Address <= Address; });
  if (It != Functions.begin() && Holds(*std::prev(It)))
    return static_cast<uint32_t>(std::prev(It) - Functions.begin());
  for (size_t I = 0; I < Functions.size(); ++I)
    if (Holds(Functions[I]))
      return static_cast<uint32_t>(I);
  return std::nullopt;
}

std::optional<std::string>
Program::validate(const ComparisonControl &Control) const {
  Control.check();
  std::string Error;
  llvm::raw_string_ostream OS(Error);
  const auto Fail = [&](const char *What, size_t Index) {
    OS << What << " (" << Index << ")";
    return Error;
  };
  for (size_t I = 0; I < Functions.size(); ++I) {
    Control.check();
    const Function &F = Functions[I];
    if (I && Functions[I - 1].Address >= F.Address)
      return Fail("functions are not in ascending address order", I);
    if (uint64_t(F.FirstBlock) + F.BlockCount > Blocks.size())
      return Fail("a function's blocks lie outside the program", I);
    if (uint64_t(F.FirstEdge) + F.EdgeCount > Edges.size())
      return Fail("a function's edges lie outside the program", I);
    if (uint64_t(F.FirstReference) + F.ReferenceCount > References.size())
      return Fail("a function's references lie outside the program", I);
    if (F.BlockCount == 0 ? F.EntryBlock != NoIndex
                          : F.EntryBlock >= F.BlockCount)
      return Fail("a function's entry block is not one of its blocks", I);
    if (F.BlockCount == 0 && F.EdgeCount != 0)
      return Fail("a function without blocks has edges", I);
    for (const Edge &E : edges(F)) {
      Control.check();
      if (E.From >= F.BlockCount || E.To >= F.BlockCount)
        return Fail("an edge leaves its function", I);
    }
    for (const Reference &R : references(F)) {
      Control.check();
      if (R.Kind > ReferenceKind::UnresolvedData)
        return Fail("a reference has an unknown kind", I);
      if (R.Block >= F.BlockCount)
        return Fail("a reference names a block outside its function", I);
    }
    for (const Block &B : blocks(F)) {
      Control.check();
      if (uint64_t(B.FirstInstruction) + B.InstructionCount >
          Instructions.size())
        return Fail("a block's instructions lie outside the program", I);
    }
  }
  for (size_t I = 0; I < Instructions.size(); ++I) {
    Control.check();
    const Instruction &Insn = Instructions[I];
    if (uint64_t(Insn.CodeOffset) + Insn.Size > Code.size())
      return Fail("an instruction's bytes lie outside the program", I);
    if (Insn.Mnemonic >= Mnemonics.size())
      return Fail("an instruction names an unknown mnemonic", I);
    if (Insn.Callee != NoIndex && Insn.Callee >= Functions.size())
      return Fail("a call names an unknown function", I);
    if (Insn.UnresolvedCallee && Insn.Callee != NoIndex)
      return Fail("a call is both resolved and unresolved", I);
    if (uint64_t(Insn.FirstAddressReference) + Insn.AddressReferenceCount >
        AddressReferences.size())
      return Fail("an instruction's address references lie outside the program",
                  I);
    const auto Addresses = addressReferences(Insn);
    for (size_t J = 1; J < Addresses.size(); ++J)
      if (Addresses[J - 1].Operand >= Addresses[J].Operand)
        return Fail("address reference operands are not in ascending order", I);
    for (const auto &Address : Addresses)
      if (Address.Kind == ReferenceKind::Constant ||
          Address.Kind > ReferenceKind::UnresolvedData)
        return Fail("an address operand has an invalid reference kind", I);
  }
  return std::nullopt;
}

ProgramBuilder::ProgramBuilder(ProgramIdentity Identity) {
  P.Identity = std::move(Identity);
}

void ProgramBuilder::endFunction() {
  if (!InFunction)
    return;
  Function &F = P.Functions.back();
  F.BlockCount = static_cast<uint32_t>(P.Blocks.size()) - F.FirstBlock;
  F.EdgeCount = static_cast<uint32_t>(P.Edges.size()) - F.FirstEdge;
  F.ReferenceCount =
      static_cast<uint32_t>(P.References.size()) - F.FirstReference;
  if (F.BlockCount && F.EntryBlock == NoIndex) {
    // The block at the entry address, else the first one added.
    F.EntryBlock = 0;
    for (uint32_t I = 0; I < F.BlockCount; ++I)
      if (P.Blocks[F.FirstBlock + I].Address == F.Address) {
        F.EntryBlock = I;
        break;
      }
  }
  // References sort by kind and key so equal sets compare equal; each keeps
  // the first block that made it.
  auto Refs = llvm::MutableArrayRef(P.References)
                  .slice(F.FirstReference, F.ReferenceCount);
  std::stable_sort(Refs.begin(), Refs.end(),
                   [](const Reference &A, const Reference &B) {
                     return std::tie(A.Kind, A.Key) < std::tie(B.Kind, B.Key);
                   });
  const auto End = std::unique(Refs.begin(), Refs.end(),
                               [](const Reference &A, const Reference &B) {
                                 return A.Kind == B.Kind && A.Key == B.Key;
                               });
  const size_t Kept = End - Refs.begin();
  P.References.resize(F.FirstReference + Kept);
  F.ReferenceCount = static_cast<uint32_t>(Kept);
  InFunction = false;
}

void ProgramBuilder::beginFunction(uint64_t Address, uint64_t Size,
                                   std::string Name, NameSource Source,
                                   uint16_t Traits, std::string Module) {
  endFunction();
  Function F;
  F.Address = Address;
  F.Size = Size;
  F.Name = std::move(Name);
  F.Module = std::move(Module);
  F.Source = Source;
  F.Traits = Traits;
  F.FirstBlock = static_cast<uint32_t>(P.Blocks.size());
  F.FirstEdge = static_cast<uint32_t>(P.Edges.size());
  F.FirstReference = static_cast<uint32_t>(P.References.size());
  P.Functions.push_back(std::move(F));
  InFunction = true;
}

uint32_t ProgramBuilder::addBlock(uint64_t Address) {
  Block B;
  B.Address = Address;
  B.FirstInstruction = static_cast<uint32_t>(P.Instructions.size());
  P.Blocks.push_back(B);
  return static_cast<uint32_t>(P.Blocks.size()) - 1 -
         P.Functions.back().FirstBlock;
}

void ProgramBuilder::addInstruction(
    uint64_t Address, llvm::ArrayRef<uint8_t> Bytes, llvm::StringRef Mnemonic,
    uint64_t Shape, uint64_t Skeleton, InstructionFlow Flow,
    std::optional<uint64_t> CallTarget, llvm::StringRef Operands,
    std::optional<uint64_t> Target, llvm::ArrayRef<AddressReference> Addresses,
    std::optional<uint64_t> AddressIndependentShape, bool HasUnresolvedData) {
  Instruction I;
  I.Address = Address;
  I.Operands = Operands.str();
  I.Target = Target;
  I.Shape = Shape;
  I.Skeleton = Skeleton;
  I.AddressIndependentShape = AddressIndependentShape.value_or(Shape);
  I.HasUnresolvedData = HasUnresolvedData;
  I.CodeOffset = static_cast<uint32_t>(P.Code.size());
  I.Size = static_cast<uint8_t>(Bytes.size());
  I.Flow = Flow;
  I.UnresolvedCallee = Flow == InstructionFlow::Call;
  I.FirstAddressReference = static_cast<uint32_t>(P.AddressReferences.size());
  I.AddressReferenceCount = static_cast<uint32_t>(Addresses.size());
  P.AddressReferences.insert(P.AddressReferences.end(), Addresses.begin(),
                             Addresses.end());
  for (const auto &A : Addresses)
    addReference(A.Kind, A.Key,
                 static_cast<uint32_t>(P.Blocks.size()) - 1 -
                     P.Functions.back().FirstBlock);
  auto [It, Inserted] = MnemonicIds.try_emplace(
      Mnemonic, static_cast<uint16_t>(P.Mnemonics.size()));
  if (Inserted)
    P.Mnemonics.push_back(Mnemonic.str());
  I.Mnemonic = It->second;
  P.Code.insert(P.Code.end(), Bytes.begin(), Bytes.end());
  if (CallTarget)
    CallTargets.emplace_back(static_cast<uint32_t>(P.Instructions.size()),
                             *CallTarget);
  P.Instructions.push_back(I);
  ++P.Blocks.back().InstructionCount;
}

void ProgramBuilder::addEdge(uint32_t From, uint32_t To, EdgeKind Kind) {
  P.Edges.push_back({From, To, Kind});
}

void ProgramBuilder::addReference(ReferenceKind Kind, uint64_t Key,
                                  uint32_t Block) {
  P.References.push_back({Kind, Key, Block});
}

void ProgramBuilder::setEntryBlock(uint32_t Block) {
  P.Functions.back().EntryBlock = Block;
}

void ProgramBuilder::addTraits(uint16_t Traits) {
  P.Functions.back().Traits |= Traits;
}

void ProgramBuilder::warn(std::string Warning) {
  P.Warnings.push_back(std::move(Warning));
}

void ProgramBuilder::setStackRoleCandidate(uint64_t Key) {
  P.Functions.back().StackRoleCandidate = Key;
}

Program ProgramBuilder::finish() {
  endFunction();
  for (const auto &[Index, Target] : CallTargets)
    if (const auto Callee = P.functionAt(Target)) {
      P.Instructions[Index].Callee = *Callee;
      P.Instructions[Index].UnresolvedCallee = false;
    } else
      P.Instructions[Index].UnresolvedCallee = true;
  CallTargets.clear();
  MnemonicIds.clear();
  return std::move(P);
}

} // namespace neverd::diff
