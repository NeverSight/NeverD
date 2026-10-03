#ifndef NEVERD_SDK_CAPI_SWIFTMANGLEDSOURCEABI_H
#define NEVERD_SDK_CAPI_SWIFTMANGLEDSOURCEABI_H

#include "../../loader/Swift/SwiftFunctionSymbols.h"
#include "../../loader/Swift/SwiftMangledClassMethodABI.h"
#include "../../loader/Swift/SwiftMangledStringBundleABI.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Demangle/SwiftDemangle.h"

#include <algorithm>
#include <array>
#include <set>
#include <vector>

namespace neverd::sdk {

// Swift's fully specialized [URL] -> [URL?] force cast carries each Array as
// one object pointer. An -O Swift compilation of _arrayForceCast on these
// exact types emits swiftcc ptr (ptr), without metadata or witness arguments.
// Require the complete specialization and generic declaration, not just its
// function name. Body, runtime-call and frame proofs remain separate gates.
inline std::optional<SourceFunctionTypeHint>
swiftMangledURLArrayForceCastSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  const auto Index = [](const Node &N, llvm::StringRef Kind, uint64_t Value) {
    return N.Kind == Kind && !N.Text && N.Index == Value && N.Children.empty();
  };
  const auto URL = [&](const Node &N) {
    return Shape(N, "Type", 1) && Shape(N.Children[0], "Structure", 2) &&
           Text(N.Children[0].Children[0], "Module", "Foundation") &&
           Text(N.Children[0].Children[1], "Identifier", "URL");
  };
  const auto OptionalURL = [&](const Node &N) {
    if (!Shape(N, "Type", 1) || !Shape(N.Children[0], "BoundGenericEnum", 2))
      return false;
    const auto &Optional = N.Children[0];
    return Shape(Optional.Children[0], "Type", 1) &&
           Shape(Optional.Children[0].Children[0], "Enum", 2) &&
           Text(Optional.Children[0].Children[0].Children[0], "Module",
                "Swift") &&
           Text(Optional.Children[0].Children[0].Children[1], "Identifier",
                "Optional") &&
           Shape(Optional.Children[1], "TypeList", 1) &&
           URL(Optional.Children[1].Children[0]);
  };
  const auto ArrayParameter = [&](const Node &N, uint64_t Parameter) {
    if (!Shape(N, "Type", 1) ||
        !Shape(N.Children[0], "BoundGenericStructure", 2))
      return false;
    const auto &Array = N.Children[0];
    if (!Shape(Array.Children[0], "Type", 1) ||
        !Shape(Array.Children[0].Children[0], "Structure", 2) ||
        !Text(Array.Children[0].Children[0].Children[0], "Module", "Swift") ||
        !Text(Array.Children[0].Children[0].Children[1], "Identifier",
              "Array") ||
        !Shape(Array.Children[1], "TypeList", 1) ||
        !Shape(Array.Children[1].Children[0], "Type", 1) ||
        !Shape(Array.Children[1].Children[0].Children[0],
               "DependentGenericParamType", 2))
      return false;
    const auto &Generic = Array.Children[1].Children[0].Children[0];
    return Index(Generic.Children[0], "Index", 0) &&
           Index(Generic.Children[1], "Index", Parameter);
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 2) ||
      !Shape(Parsed.Root->Children[0], "GenericSpecialization", 3) ||
      !Shape(Parsed.Root->Children[1], "Function", 4))
    return std::nullopt;
  const auto &Specialization = Parsed.Root->Children[0];
  if (!Index(Specialization.Children[0], "SpecializationPassID", 5) ||
      !Shape(Specialization.Children[1], "GenericSpecializationParam", 1) ||
      !Shape(Specialization.Children[2], "GenericSpecializationParam", 1) ||
      !URL(Specialization.Children[1].Children[0]) ||
      !OptionalURL(Specialization.Children[2].Children[0]))
    return std::nullopt;
  const auto &Function = Parsed.Root->Children[1];
  if (!Text(Function.Children[0], "Module", "Swift") ||
      !Text(Function.Children[1], "Identifier", "_arrayForceCast") ||
      !Shape(Function.Children[2], "LabelList", 0) ||
      !Shape(Function.Children[3], "Type", 1) ||
      !Shape(Function.Children[3].Children[0], "DependentGenericType", 2))
    return std::nullopt;
  const auto &Generic = Function.Children[3].Children[0];
  if (!Shape(Generic.Children[0], "DependentGenericSignature", 1) ||
      !Index(Generic.Children[0].Children[0], "DependentGenericParamCount",
             2) ||
      !Shape(Generic.Children[1], "Type", 1) ||
      !Shape(Generic.Children[1].Children[0], "FunctionType", 2))
    return std::nullopt;
  const auto &Type = Generic.Children[1].Children[0];
  if (!Shape(Type.Children[0], "ArgumentTuple", 1) ||
      !ArrayParameter(Type.Children[0].Children[0], 0) ||
      !Shape(Type.Children[1], "ReturnType", 1) ||
      !ArrayParameter(Type.Children[1].Children[0], 1))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"array", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// The fully specialized ContiguousArray<URL?> buffer initializer has three
// scalar arguments and an inout, one-word Array in swiftself. Independent
// Swift IR carries these as i1, i64, i1, ptr swiftself, with no return value.
// Match the entire declaration and specialization; this supplies only its
// ABI, while the caller's frame effects and callee body still need proof.
inline std::optional<SourceFunctionTypeHint>
swiftMangledURLArrayBufferSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  const auto Nominal = [&](const Node &N, llvm::StringRef Module,
                           llvm::StringRef Identifier) {
    return Shape(N, "Type", 1) && Shape(N.Children[0], "Structure", 2) &&
           Text(N.Children[0].Children[0], "Module", Module) &&
           Text(N.Children[0].Children[1], "Identifier", Identifier);
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 2) ||
      !Shape(Parsed.Root->Children[0], "GenericSpecialization", 2) ||
      !Shape(Parsed.Root->Children[1], "Function", 4))
    return std::nullopt;
  const auto &Specialization = Parsed.Root->Children[0];
  if (Specialization.Children[0].Kind != "SpecializationPassID" ||
      Specialization.Children[0].Text ||
      Specialization.Children[0].Index != 5 ||
      !Specialization.Children[0].Children.empty() ||
      !Shape(Specialization.Children[1], "GenericSpecializationParam", 1))
    return std::nullopt;
  const auto &Element = Specialization.Children[1].Children[0];
  if (!Shape(Element, "Type", 1) ||
      !Shape(Element.Children[0], "BoundGenericEnum", 2))
    return std::nullopt;
  const auto &Optional = Element.Children[0];
  if (!Shape(Optional.Children[0], "Type", 1) ||
      !Shape(Optional.Children[0].Children[0], "Enum", 2) ||
      !Text(Optional.Children[0].Children[0].Children[0], "Module", "Swift") ||
      !Text(Optional.Children[0].Children[0].Children[1], "Identifier",
            "Optional") ||
      !Shape(Optional.Children[1], "TypeList", 1) ||
      !Nominal(Optional.Children[1].Children[0], "Foundation", "URL"))
    return std::nullopt;
  const auto &Function = Parsed.Root->Children[1];
  if (!Shape(Function.Children[0], "Structure", 2) ||
      !Text(Function.Children[0].Children[0], "Module", "Swift") ||
      !Text(Function.Children[0].Children[1], "Identifier",
            "ContiguousArray") ||
      !Text(Function.Children[1], "Identifier", "_createNewBuffer") ||
      !Shape(Function.Children[2], "LabelList", 3) ||
      !Text(Function.Children[2].Children[0], "Identifier", "bufferIsUnique") ||
      !Text(Function.Children[2].Children[1], "Identifier",
            "minimumCapacity") ||
      !Text(Function.Children[2].Children[2], "Identifier", "growForAppend") ||
      !Shape(Function.Children[3], "Type", 1) ||
      !Shape(Function.Children[3].Children[0], "FunctionType", 2))
    return std::nullopt;
  const auto &Type = Function.Children[3].Children[0];
  if (!Shape(Type.Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Tuple", 3) ||
      !Shape(Type.Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[1].Children[0].Children[0], "Tuple", 0))
    return std::nullopt;
  const auto &Tuple = Type.Children[0].Children[0].Children[0];
  for (size_t I = 0; I < 3; ++I)
    if (!Shape(Tuple.Children[I], "TupleElement", 1) ||
        !Nominal(Tuple.Children[I].Children[0], "Swift",
                 I == 1 ? "Int" : "Bool"))
      return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeVoid();
  Hint.Parameters = {{"buffer_is_unique", NdType::makeInt(1, false)},
                     {"minimum_capacity", NdType::makeInt(8, true)},
                     {"grow_for_append", NdType::makeInt(1, false)},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters.back().TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// The optimized compiler emits this exact specialization as a method taking
// an owned one-word Array value in X0 and an inout Array cell in swiftself X20.
// Compare the entire demangled declaration, including both specializations,
// the Sequence/Element requirements, ownership, labels and void result.
// This declares its ABI; native body and dependency proofs remain required.
inline std::optional<SourceFunctionTypeHint>
swiftMangledURLArrayAppendSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  const auto Expected = llvm::swiftDemangle(
      "$sSa6append10contentsOfyqd__n_t7ElementQyd__RszSTRd__lF"
      "10Foundation3URLVSg_SayAHGTg5",
      Options);
  if (!Parsed.Root || !Parsed.Error.empty() || !Expected.Root ||
      !Expected.Error.empty())
    return std::nullopt;
  size_t Budget = 128;
  const auto Equal = [&](const auto &Self, const llvm::SwiftDemangleNode &A,
                         const llvm::SwiftDemangleNode &B) -> bool {
    if (!Budget || A.Kind != B.Kind || A.Text != B.Text || A.Index != B.Index ||
        A.Children.size() != B.Children.size())
      return false;
    --Budget;
    for (size_t I = 0; I < A.Children.size(); ++I)
      if (!Self(Self, A.Children[I], B.Children[I]))
        return false;
    return true;
  };
  if (!Equal(Equal, *Parsed.Root, *Expected.Root))
    return std::nullopt;
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeVoid();
  Hint.Parameters = {
      {"source_array", NdType::makePtr(NdType::makeVoid())},
      {"destination_array", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters.back().TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Diagnostic;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Diagnostic)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// A Swift value initializer may return its Array<String> argument unchanged
// in x0 while still performing observable side effects. The mangled type
// gives the nominal and argument shape; the complete single-block
// LowIR proves that the incoming word reaches the return without a write or
// call clobber. This is deliberately limited to the one-word array carrier.
inline std::optional<SourceFunctionTypeHint>
swiftMangledArrayStringValueInitializerSourceABI(
    const BinaryImage &Image, va_t Entry, const LowFunc &Low,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry) || Low.Entry != Entry ||
      Low.Blocks.size() != 1 || Low.Blocks[0].StartAddr != Entry ||
      !Low.Blocks[0].Preds.empty() || !Low.Blocks[0].Succs.empty() ||
      Low.Blocks[0].Ops.empty() || Low.Blocks[0].Ops.size() > 4096)
    return std::nullopt;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const auto Register = TRI.IntReturnReg;
  const auto &Ops = Low.Blocks[0].Ops;
  const auto &Return = Ops.back();
  if (Return.Opcode != NdOp::RETURN || Return.NumInputs != 1 ||
      !Return.Inputs[0].isReg() ||
      Return.Inputs[0].Offset != TRI.LinkRegister || Return.Inputs[0].Size != 8)
    return std::nullopt;
  for (size_t I = 0; I + 1 < Ops.size(); ++I) {
    const auto &Op = Ops[I];
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
        Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
        Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::INTRINSIC ||
        Op.Opcode == NdOp::RETURN ||
        (Op.Output.isReg() && Op.Output.Size &&
         (Op.Output.Offset <= Register
              ? Register - Op.Output.Offset < Op.Output.Size
              : Op.Output.Offset - Register < 8)))
      return std::nullopt;
  }

  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Allocator", 3))
    return std::nullopt;
  const auto &Allocator = Parsed.Root->Children[0];
  const auto &Nominal = Allocator.Children[0];
  const auto &Labels = Allocator.Children[1];
  const auto &Type = Allocator.Children[2];
  if (!Shape(Nominal, "Structure", 2) || Nominal.Children[0].Kind != "Module" ||
      !Nominal.Children[0].Text || Nominal.Children[0].Text->empty() ||
      Nominal.Children[0].Index || !Nominal.Children[0].Children.empty() ||
      Nominal.Children[1].Kind != "Identifier" || !Nominal.Children[1].Text ||
      Nominal.Children[1].Text->empty() || Nominal.Children[1].Index ||
      !Nominal.Children[1].Children.empty() || !Shape(Labels, "LabelList", 1) ||
      Labels.Children[0].Kind != "Identifier" || !Labels.Children[0].Text ||
      Labels.Children[0].Text->empty() || Labels.Children[0].Index ||
      !Labels.Children[0].Children.empty() || !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             1) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0].Children[0], "Structure",
             2))
    return std::nullopt;
  const auto &Result = Type.Children[0].Children[1].Children[0].Children[0];
  if (!Text(Result.Children[0], "Module", *Nominal.Children[0].Text) ||
      !Text(Result.Children[1], "Identifier", *Nominal.Children[1].Text))
    return std::nullopt;
  const auto &Element =
      Type.Children[0].Children[0].Children[0].Children[0].Children[0];
  if (!Shape(Element, "TupleElement", 1) ||
      !Shape(Element.Children[0], "Type", 1) ||
      !Shape(Element.Children[0].Children[0], "BoundGenericStructure", 2))
    return std::nullopt;
  const auto &Array = Element.Children[0].Children[0];
  if (!Shape(Array.Children[0], "Type", 1) ||
      !Shape(Array.Children[0].Children[0], "Structure", 2) ||
      !Text(Array.Children[0].Children[0].Children[0], "Module", "Swift") ||
      !Text(Array.Children[0].Children[0].Children[1], "Identifier", "Array") ||
      !Shape(Array.Children[1], "TypeList", 1) ||
      !Shape(Array.Children[1].Children[0], "Type", 1) ||
      !Shape(Array.Children[1].Children[0].Children[0], "Structure", 2) ||
      !Text(Array.Children[1].Children[0].Children[0].Children[0], "Module",
            "Swift") ||
      !Text(Array.Children[1].Children[0].Children[0].Children[1], "Identifier",
            "String"))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"array", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// Keep the SDK call site on the single loader-owned ABI declaration.
using neverd::swiftMangledStringBundleSourceABI;

// A Swift extension Bool getter or zero-argument method on an Objective-C
// class carries its receiver in swiftself. The closed mangled type supplies
// the return width; the source pipeline must still prove the body and every
// call before publication.
inline std::optional<SourceFunctionTypeHint>
swiftMangledObjCBoolMemberSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1))
    return std::nullopt;
  const auto &Root = Parsed.Root->Children[0];
  const Node *Member = nullptr, *Result = nullptr;
  if (Shape(Root, "Getter", 1) && Shape(Root.Children[0], "Variable", 3)) {
    Member = &Root.Children[0];
    if (Shape(Member->Children[2], "Type", 1))
      Result = &Member->Children[2].Children[0];
  } else if (Shape(Root, "Function", 3)) {
    Member = &Root;
    const auto &Type = Root.Children[2];
    if (Shape(Type, "Type", 1) && Shape(Type.Children[0], "FunctionType", 2) &&
        Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) &&
        Shape(Type.Children[0].Children[0].Children[0], "Type", 1) &&
        Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
              0) &&
        Shape(Type.Children[0].Children[1], "ReturnType", 1) &&
        Shape(Type.Children[0].Children[1].Children[0], "Type", 1))
      Result = &Type.Children[0].Children[1].Children[0].Children[0];
  }
  if (!Member || !Result || !Shape(Member->Children[0], "Extension", 2) ||
      Member->Children[0].Children[0].Kind != "Module" ||
      !Member->Children[0].Children[0].Text ||
      Member->Children[0].Children[0].Text->empty() ||
      Member->Children[0].Children[0].Index ||
      !Member->Children[0].Children[0].Children.empty() ||
      !Shape(Member->Children[0].Children[1], "Class", 2) ||
      !Text(Member->Children[0].Children[1].Children[0], "Module", "__C") ||
      Member->Children[0].Children[1].Children[1].Kind != "Identifier" ||
      !Member->Children[0].Children[1].Children[1].Text ||
      Member->Children[0].Children[1].Children[1].Text->empty() ||
      Member->Children[0].Children[1].Children[1].Index ||
      !Member->Children[0].Children[1].Children[1].Children.empty() ||
      Member->Children[1].Kind != "Identifier" || !Member->Children[1].Text ||
      Member->Children[1].Text->empty() || Member->Children[1].Index ||
      !Member->Children[1].Children.empty() ||
      !Shape(*Result, "Structure", 2) ||
      !Text(Result->Children[0], "Module", "Swift") ||
      !Text(Result->Children[1], "Identifier", "Bool"))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeInt(1, false);
  Hint.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// A direct Swift class method with one Objective-C object argument uses x0 for
// that argument and swiftself for the receiver. ObjC thunks and merged
// functions have different entry contracts and are deliberately excluded.
inline std::optional<SourceFunctionTypeHint>
swiftMangledObjCObjectVoidMethodSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Function", 4))
    return std::nullopt;
  const auto &Function = Parsed.Root->Children[0];
  const auto &Owner = Function.Children[0];
  const auto &Labels = Function.Children[2];
  const auto &Type = Function.Children[3];
  const bool OneNamedArgument =
      Shape(Labels, "LabelList", 1) &&
      Labels.Children[0].Kind == "Identifier" && Labels.Children[0].Text &&
      !Labels.Children[0].Text->empty() && !Labels.Children[0].Index &&
      Labels.Children[0].Children.empty();
  if (!Shape(Owner, "Class", 2) || Owner.Children[0].Kind != "Module" ||
      !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
      Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
      Owner.Children[1].Kind != "Identifier" || !Owner.Children[1].Text ||
      Owner.Children[1].Text->empty() || Owner.Children[1].Index ||
      !Owner.Children[1].Children.empty() ||
      Function.Children[1].Kind != "Identifier" || !Function.Children[1].Text ||
      Function.Children[1].Text->empty() || Function.Children[1].Index ||
      !Function.Children[1].Children.empty() ||
      !(Shape(Labels, "LabelList", 0) || OneNamedArgument) ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0].Children[0], "Tuple", 0))
    return std::nullopt;
  const auto &RawArgument =
      Type.Children[0].Children[0].Children[0].Children[0];
  const Node *Argument = nullptr;
  if (Shape(Labels, "LabelList", 0) && Shape(RawArgument, "Class", 2))
    Argument = &RawArgument;
  else if (OneNamedArgument && Shape(RawArgument, "Tuple", 1) &&
           Shape(RawArgument.Children[0], "TupleElement", 1) &&
           Shape(RawArgument.Children[0].Children[0], "Type", 1) &&
           Shape(RawArgument.Children[0].Children[0].Children[0], "Class", 2))
    Argument = &RawArgument.Children[0].Children[0].Children[0];
  if (!Argument)
    return std::nullopt;
  const auto &ArgumentName = Argument->Children[1];
  if (!Text(Argument->Children[0], "Module", "__C") ||
      ArgumentName.Kind != "Identifier" || !ArgumentName.Text ||
      ArgumentName.Text->empty() || ArgumentName.Index ||
      !ArgumentName.Children.empty())
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeVoid();
  Hint.Parameters = {{"object", NdType::makePtr(NdType::makeVoid())},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[1].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// Discovery and call qualification share this exact loader-owned declaration.
using neverd::swiftMangledObjCObjectPairVoidMethodSourceABI;

// The unlabeled UIColor extension allocator has an Int value in x0 and the
// class metadata in swiftself. The demangled argument must be Swift.Int;
// matching a machine-width integer carrier alone would also accept pointers.
inline std::optional<SourceFunctionTypeHint>
swiftMangledUIColorIntAllocatorSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  const auto UIColor = [&](const Node &N) {
    return Shape(N, "Class", 2) && Text(N.Children[0], "Module", "__C") &&
           Text(N.Children[1], "Identifier", "UIColor");
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Allocator", 3))
    return std::nullopt;
  const auto &Allocator = Parsed.Root->Children[0];
  const auto &Owner = Allocator.Children[0];
  const auto &Labels = Allocator.Children[1];
  const auto &Type = Allocator.Children[2];
  if (!Shape(Owner, "Extension", 2) || Owner.Children[0].Kind != "Module" ||
      !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
      Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
      !UIColor(Owner.Children[1]) || !Shape(Labels, "LabelList", 0) ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Structure",
             2) ||
      !Text(Type.Children[0].Children[0].Children[0].Children[0].Children[0],
            "Module", "Swift") ||
      !Text(Type.Children[0].Children[0].Children[0].Children[0].Children[1],
            "Identifier", "Int") ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !UIColor(Type.Children[0].Children[1].Children[0].Children[0]))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"color", NdType::makeInt(8, true)},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[1].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// The UIColor extension allocator taking Int and CGFloat has one integer,
// one double, and the UIColor class metadata in swiftself. A compiler probe
// for the corresponding NSColor extension confirms the mixed register layout.
inline std::optional<SourceFunctionTypeHint>
swiftMangledUIColorIntAlphaAllocatorSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  const auto UIColor = [&](const Node &N) {
    return Shape(N, "Class", 2) && Text(N.Children[0], "Module", "__C") &&
           Text(N.Children[1], "Identifier", "UIColor");
  };
  const auto Nominal = [&](const Node &N, llvm::StringRef Module,
                           llvm::StringRef Identifier) {
    return Shape(N, "Structure", 2) && Text(N.Children[0], "Module", Module) &&
           Text(N.Children[1], "Identifier", Identifier);
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Allocator", 3))
    return std::nullopt;
  const auto &Allocator = Parsed.Root->Children[0];
  const auto &Owner = Allocator.Children[0];
  const auto &Labels = Allocator.Children[1];
  const auto &Type = Allocator.Children[2];
  if (!Shape(Owner, "Extension", 2) || Owner.Children[0].Kind != "Module" ||
      !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
      Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
      !UIColor(Owner.Children[1]) || !Shape(Labels, "LabelList", 2) ||
      !Shape(Labels.Children[0], "FirstElementMarker", 0) ||
      !Text(Labels.Children[1], "Identifier", "alpha") ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             2) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !UIColor(Type.Children[0].Children[1].Children[0].Children[0]))
    return std::nullopt;
  const auto &Arguments =
      Type.Children[0].Children[0].Children[0].Children[0].Children;
  for (const auto &Argument : Arguments)
    if (!Shape(Argument, "TupleElement", 1) ||
        !Shape(Argument.Children[0], "Type", 1))
      return std::nullopt;
  if (!Nominal(Arguments[0].Children[0].Children[0], "Swift", "Int") ||
      !Nominal(Arguments[1].Children[0].Children[0], "CoreGraphics", "CGFloat"))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"color", NdType::makeInt(8, true)},
                     {"alpha", NdType::makeFloat(8)},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[2].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// A zero-argument Swift class instance method returning Void or Bool receives
// only its object in swiftself. Reject static, generic, extension, and ObjC
// thunk wrappers: their root mangling shapes are distinct from this body.
using neverd::swiftMangledZeroArgClassMethodSourceABI;

// A direct Swift class getter for Optional<any P> writes its existential
// result through x8 and takes the class instance in swiftself. The complete
// mangled tree excludes class-constrained and protocol-composition layouts.
inline std::optional<SourceFunctionTypeHint>
swiftMangledClassOptionalExistentialGetterSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Identifier = [&](const Node &N, llvm::StringRef Kind) {
    return N.Kind == Kind && N.Text && !N.Text->empty() && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Getter", 1) ||
      !Shape(Parsed.Root->Children[0].Children[0], "Variable", 3))
    return std::nullopt;
  const auto &Variable = Parsed.Root->Children[0].Children[0];
  const auto &Owner = Variable.Children[0];
  const auto &Type = Variable.Children[2];
  if (!Shape(Owner, "Class", 2) || !Identifier(Owner.Children[0], "Module") ||
      !Identifier(Owner.Children[1], "Identifier") ||
      !Identifier(Variable.Children[1], "Identifier") ||
      !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "BoundGenericEnum", 2))
    return std::nullopt;
  const auto &Optional = Type.Children[0];
  if (!Shape(Optional.Children[0], "Type", 1) ||
      !Shape(Optional.Children[0].Children[0], "Enum", 2) ||
      !Identifier(Optional.Children[0].Children[0].Children[0], "Module") ||
      *Optional.Children[0].Children[0].Children[0].Text != "Swift" ||
      !Identifier(Optional.Children[0].Children[0].Children[1], "Identifier") ||
      *Optional.Children[0].Children[0].Children[1].Text != "Optional" ||
      !Shape(Optional.Children[1], "TypeList", 1) ||
      !Shape(Optional.Children[1].Children[0], "Type", 1) ||
      !Shape(Optional.Children[1].Children[0].Children[0], "ProtocolList", 1) ||
      !Shape(Optional.Children[1].Children[0].Children[0].Children[0],
             "TypeList", 1))
    return std::nullopt;
  const auto &ProtocolType =
      Optional.Children[1].Children[0].Children[0].Children[0].Children[0];
  if (!Shape(ProtocolType, "Type", 1) ||
      !Shape(ProtocolType.Children[0], "Protocol", 2) ||
      !Identifier(ProtocolType.Children[0].Children[0], "Module") ||
      !Identifier(ProtocolType.Children[0].Children[1], "Identifier"))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeVoid();
  Hint.Parameters = {{"result", NdType::makePtr(NdType::makeVoid())},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[0].TheRole =
      SourceParameterTypeHint::Role::SwiftIndirectResult;
  Hint.Parameters[1].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// A direct Swift class property getter takes only swiftself. The complete
// mangled tree determines whether its result is Bool, Int, or arm64 CGFloat;
// ObjC thunk suffixes and extensions have separate entry contracts.
inline std::optional<SourceFunctionTypeHint>
swiftMangledClassScalarGetterSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Getter", 1) ||
      !Shape(Parsed.Root->Children[0].Children[0], "Variable", 3))
    return std::nullopt;
  const auto &Variable = Parsed.Root->Children[0].Children[0];
  const auto &Owner = Variable.Children[0];
  const auto &Property = Variable.Children[1];
  const auto &Type = Variable.Children[2];
  if (!Shape(Owner, "Class", 2) || Owner.Children[0].Kind != "Module" ||
      !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
      Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
      Owner.Children[1].Kind != "Identifier" || !Owner.Children[1].Text ||
      Owner.Children[1].Text->empty() || Owner.Children[1].Index ||
      !Owner.Children[1].Children.empty() || Property.Kind != "Identifier" ||
      !Property.Text || Property.Text->empty() || Property.Index ||
      !Property.Children.empty() || !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "Structure", 2))
    return std::nullopt;
  const auto &Value = Type.Children[0];
  const bool Swift = Text(Value.Children[0], "Module", "Swift");
  const bool IsBool = Swift && Text(Value.Children[1], "Identifier", "Bool");
  const bool IsInt = Swift && Text(Value.Children[1], "Identifier", "Int");
  const bool IsCGFloat = Text(Value.Children[0], "Module", "CoreGraphics") &&
                         Text(Value.Children[1], "Identifier", "CGFloat");
  if (!IsBool && !IsInt && !IsCGFloat)
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = IsCGFloat ? NdType::makeFloat(8)
                              : NdType::makeInt(IsBool ? 1 : 8, !IsBool);
  Hint.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// Swift 6.1 arm64 emits a direct Double extension getter returning CGFloat as
// swiftcc double(double), and a nongeneric nested value's Double property
// initializer as swiftcc double(). Both use the FP return lane. The complete
// mangled tree excludes thunks, generic substitutions, and other value layouts.
inline std::optional<SourceFunctionTypeHint>
swiftMangledDoubleFloatingPropertySourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  const auto Identifier = [](const Node &N) {
    return N.Kind == "Identifier" && N.Text && !N.Text->empty() && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1))
    return std::nullopt;
  const auto &Accessor = Parsed.Root->Children[0];
  if ((!Shape(Accessor, "Getter", 1) && !Shape(Accessor, "Initializer", 1)) ||
      !Shape(Accessor.Children[0], "Variable", 3))
    return std::nullopt;
  const auto &Variable = Accessor.Children[0];
  if (!Identifier(Variable.Children[1]) ||
      !Shape(Variable.Children[2], "Type", 1) ||
      !Shape(Variable.Children[2].Children[0], "Structure", 2))
    return std::nullopt;
  const auto &Result = Variable.Children[2].Children[0];
  const auto &Owner = Variable.Children[0];
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeFloat(8);
  if (Accessor.Kind == "Getter") {
    if (!Shape(Owner, "Extension", 2) || Owner.Children[0].Kind != "Module" ||
        !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
        Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
        !Shape(Owner.Children[1], "Structure", 2) ||
        !Text(Owner.Children[1].Children[0], "Module", "Swift") ||
        !Text(Owner.Children[1].Children[1], "Identifier", "Double") ||
        !Text(Result.Children[0], "Module", "CoreGraphics") ||
        !Text(Result.Children[1], "Identifier", "CGFloat"))
      return std::nullopt;
    Hint.Parameters = {{"self", NdType::makeFloat(8)}};
  } else {
    // A generic outer class has the same accessor tree but adds a hidden
    // metadata argument. The mangling alone cannot exclude that shape.
    if (Name != "$s6Lottie18CoreAnimationLayerC26CAMediaTimingConfigurationV"
                "10timeOffsetSdvpfi")
      return std::nullopt;
    if (!Shape(Owner, "Structure", 2) ||
        !Shape(Owner.Children[0], "Class", 2) ||
        Owner.Children[0].Children[0].Kind != "Module" ||
        !Owner.Children[0].Children[0].Text ||
        Owner.Children[0].Children[0].Text->empty() ||
        Owner.Children[0].Children[0].Index ||
        !Owner.Children[0].Children[0].Children.empty() ||
        !Identifier(Owner.Children[0].Children[1]) ||
        !Identifier(Owner.Children[1]) ||
        !Text(Result.Children[0], "Module", "Swift") ||
        !Text(Result.Children[1], "Identifier", "Double"))
      return std::nullopt;
  }
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// Swift 6.1 arm64 IR gives a direct class-instance getter returning either a
// Swift or imported ObjC class reference swiftcc ptr(ptr swiftself). Keep the
// closed mangled Class result; optional, generic and thunk results have
// distinct contracts.
inline std::optional<SourceFunctionTypeHint>
swiftMangledClassReferenceGetterSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Named = [](const Node &N, llvm::StringRef Kind) {
    return N.Kind == Kind && N.Text && !N.Text->empty() && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Getter", 1) ||
      !Shape(Parsed.Root->Children[0].Children[0], "Variable", 3))
    return std::nullopt;
  const auto &Variable = Parsed.Root->Children[0].Children[0];
  const auto &Owner = Variable.Children[0];
  const auto &Property = Variable.Children[1];
  const auto &Type = Variable.Children[2];
  const bool NamedProperty = Named(Property, "Identifier") ||
                             (Shape(Property, "PrivateDeclName", 2) &&
                              Named(Property.Children[0], "Identifier") &&
                              Named(Property.Children[1], "Identifier"));
  if (!Shape(Owner, "Class", 2) || !Named(Owner.Children[0], "Module") ||
      !Named(Owner.Children[1], "Identifier") || !NamedProperty ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "Class", 2) ||
      !Named(Type.Children[0].Children[0], "Module") ||
      !Named(Type.Children[0].Children[1], "Identifier"))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// Swift 6.1 whole-module optimization merges the @objc CGFloat setters of
// WMF.AlignedImageButton. With profile instrumentation, the merged C-ABI
// helper takes self, _cmd, the new double, an ivar-offset pointer, and a
// profile-counter pointer. The two latter operands are explicit parameters
// even though the mangling describes only the source property. Keep this
// compiler-observed contract restricted to the exact profiled helper.
inline std::optional<SourceFunctionTypeHint>
swiftMangledProfiledObjCCGFloatSetterSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (Name != "$s3WMF18AlignedImageButtonC17horizontalSpacing12CoreGraphics"
              "7CGFloatVvsToTm")
    return std::nullopt;
  // The profiler-only fifth operand is absent from the mangling. Require
  // this helper's machine entry to copy x3, then increment the cell through
  // that copy before admitting the five-parameter variant.
  const uint8_t *Code = Image.readVA(Entry, 0x4c);
  constexpr std::array<uint8_t, 4> ProfileInput = {0xf3, 0x03, 0x03, 0xaa};
  constexpr std::array<uint8_t, 12> ProfileUpdate = {
      0x68, 0x02, 0x40, 0xf9, 0x08, 0x05, 0x00, 0x91, 0x68, 0x02, 0x00, 0xf9};
  if (!Code ||
      !std::equal(ProfileInput.begin(), ProfileInput.end(), Code + 0x18) ||
      !std::equal(ProfileUpdate.begin(), ProfileUpdate.end(), Code + 0x40))
    return std::nullopt;

  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 3) ||
      !Shape(Parsed.Root->Children[0], "MergedFunction", 0) ||
      !Shape(Parsed.Root->Children[1], "ObjCAttribute", 0) ||
      !Shape(Parsed.Root->Children[2], "Setter", 1) ||
      !Shape(Parsed.Root->Children[2].Children[0], "Variable", 3))
    return std::nullopt;
  const auto &Variable = Parsed.Root->Children[2].Children[0];
  if (!Shape(Variable.Children[0], "Class", 2) ||
      !Text(Variable.Children[0].Children[0], "Module", "WMF") ||
      !Text(Variable.Children[0].Children[1], "Identifier",
            "AlignedImageButton") ||
      !Text(Variable.Children[1], "Identifier", "horizontalSpacing") ||
      !Shape(Variable.Children[2], "Type", 1) ||
      !Shape(Variable.Children[2].Children[0], "Structure", 2) ||
      !Text(Variable.Children[2].Children[0].Children[0], "Module",
            "CoreGraphics") ||
      !Text(Variable.Children[2].Children[0].Children[1], "Identifier",
            "CGFloat"))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeVoid();
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"self", Pointer},
                     {"selector", Pointer},
                     {"value", NdType::makeFloat(8)},
                     {"ivarOffset", Pointer},
                     {"profileCounter", Pointer}};
  std::string Error;
  return assignDarwinFixedSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// A Swift class Bool or Int property setter takes its new value in x0 and the
// instance in swiftself. Its mangled Setter/Variable tree distinguishes the
// void result from the Bool, Int, or arm64 CGFloat property type; generic
// register-result inference must not treat a clobbered x0 as a setter return.
inline std::optional<SourceFunctionTypeHint>
swiftMangledClassScalarSetterSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Setter", 1) ||
      !Shape(Parsed.Root->Children[0].Children[0], "Variable", 3))
    return std::nullopt;
  const auto &Variable = Parsed.Root->Children[0].Children[0];
  const auto &Owner = Variable.Children[0];
  const auto &Property = Variable.Children[1];
  const auto &Type = Variable.Children[2];
  if (!Shape(Owner, "Class", 2) || Owner.Children[0].Kind != "Module" ||
      !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
      Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
      Owner.Children[1].Kind != "Identifier" || !Owner.Children[1].Text ||
      Owner.Children[1].Text->empty() || Owner.Children[1].Index ||
      !Owner.Children[1].Children.empty() || Property.Kind != "Identifier" ||
      !Property.Text || Property.Text->empty() || Property.Index ||
      !Property.Children.empty() || !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "Structure", 2))
    return std::nullopt;
  const auto &Value = Type.Children[0];
  const bool Swift = Text(Value.Children[0], "Module", "Swift");
  const bool IsBool = Swift && Text(Value.Children[1], "Identifier", "Bool");
  const bool IsInt = Swift && Text(Value.Children[1], "Identifier", "Int");
  const bool IsCGFloat = Text(Value.Children[0], "Module", "CoreGraphics") &&
                         Text(Value.Children[1], "Identifier", "CGFloat");
  if (!IsBool && !IsInt && !IsCGFloat)
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeVoid();
  Hint.Parameters = {{"value", IsCGFloat
                                   ? NdType::makeFloat(8)
                                   : NdType::makeInt(IsBool ? 1 : 8, !IsBool)},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[1].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// An initializing Swift class constructor taking Any receives a pointer to
// existential storage in x0 and its allocated receiver in swiftself. Match
// the complete unspecialized constructor type; an allocating entry or ObjC
// thunk has a different contract.
inline std::optional<SourceFunctionTypeHint>
swiftMangledAnyClassInitializerSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Constructor", 3))
    return std::nullopt;
  const auto &Constructor = Parsed.Root->Children[0];
  const auto &Class = Constructor.Children[0];
  const auto &Labels = Constructor.Children[1];
  const auto &Type = Constructor.Children[2];
  if (!Shape(Class, "Class", 2) || Class.Children[0].Kind != "Module" ||
      !Class.Children[0].Text || Class.Children[0].Text->empty() ||
      Class.Children[0].Index || !Class.Children[0].Children.empty() ||
      Class.Children[1].Kind != "Identifier" || !Class.Children[1].Text ||
      Class.Children[1].Text->empty() || Class.Children[1].Index ||
      !Class.Children[1].Children.empty() || !Shape(Labels, "LabelList", 1) ||
      !Text(Labels.Children[0], "Identifier", "layer") ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             1) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0].Children[0], "Class", 2))
    return std::nullopt;
  const auto &Argument =
      Type.Children[0].Children[0].Children[0].Children[0].Children[0];
  if (!Shape(Argument, "TupleElement", 1) ||
      !Shape(Argument.Children[0], "Type", 1) ||
      !Shape(Argument.Children[0].Children[0], "ProtocolList", 1) ||
      !Shape(Argument.Children[0].Children[0].Children[0], "TypeList", 0))
    return std::nullopt;
  const auto &Result = Type.Children[0].Children[1].Children[0].Children[0];
  if (!Text(Result.Children[0], "Module", *Class.Children[0].Text) ||
      !Text(Result.Children[1], "Identifier", *Class.Children[1].Text))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"value", NdType::makePtr(NdType::makeVoid())},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[1].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// An Objective-C class extension getter for Optional<String> carries its
// receiver in swiftself and returns the two-word String payload in x0/x1.
// Optional uses String's spare bits, so it has the same two-word carrier.
// The exact local mangled symbol supplies the type; body and caller closure
// remain separate source-projection requirements.
inline std::optional<SourceFunctionTypeHint>
swiftMangledObjCOptionalStringGetterSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Getter", 1) ||
      !Shape(Parsed.Root->Children[0].Children[0], "Variable", 3))
    return std::nullopt;
  const auto &Variable = Parsed.Root->Children[0].Children[0];
  const auto &Extension = Variable.Children[0];
  const auto &Property = Variable.Children[1];
  const auto &Type = Variable.Children[2];
  if (!Shape(Extension, "Extension", 2) ||
      Extension.Children[0].Kind != "Module" || !Extension.Children[0].Text ||
      Extension.Children[0].Text->empty() || Extension.Children[0].Index ||
      !Extension.Children[0].Children.empty() ||
      !Shape(Extension.Children[1], "Class", 2) ||
      !Text(Extension.Children[1].Children[0], "Module", "__C") ||
      Extension.Children[1].Children[1].Kind != "Identifier" ||
      !Extension.Children[1].Children[1].Text ||
      Extension.Children[1].Children[1].Text->empty() ||
      Extension.Children[1].Children[1].Index ||
      !Extension.Children[1].Children[1].Children.empty() ||
      Property.Kind != "Identifier" || !Property.Text ||
      Property.Text->empty() || Property.Index || !Property.Children.empty() ||
      !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "BoundGenericEnum", 2))
    return std::nullopt;
  const auto &Optional = Type.Children[0];
  if (!Shape(Optional.Children[0], "Type", 1) ||
      !Shape(Optional.Children[0].Children[0], "Enum", 2) ||
      !Text(Optional.Children[0].Children[0].Children[0], "Module", "Swift") ||
      !Text(Optional.Children[0].Children[0].Children[1], "Identifier",
            "Optional") ||
      !Shape(Optional.Children[1], "TypeList", 1) ||
      !Shape(Optional.Children[1].Children[0], "Type", 1) ||
      !Shape(Optional.Children[1].Children[0].Children[0], "Structure", 2) ||
      !Text(Optional.Children[1].Children[0].Children[0].Children[0], "Module",
            "Swift") ||
      !Text(Optional.Children[1].Children[0].Children[0].Children[1],
            "Identifier", "String"))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeStruct(
      {NdType::makeInt(8, false), NdType::makePtr(NdType::makeVoid())});
  Hint.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// An ObjC class extension getter for Optional<UInt64> returns its payload
// in x0 and the enum tag in x1. An optional imported ObjC class instead uses
// its null pointer as the empty case and returns only x0, including when the
// getter belongs to a Swift class. Both receivers live in swiftself. Match
// the complete mangled result so no other optional value layout inherits
// either register contract.
inline std::optional<SourceFunctionTypeHint>
swiftMangledObjCOptionalUInt64OrClassGetterSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  const auto Nominal = [&](const Node &N, llvm::StringRef Kind,
                           llvm::StringRef Module, llvm::StringRef Name) {
    return Shape(N, Kind, 2) && Text(N.Children[0], "Module", Module) &&
           Text(N.Children[1], "Identifier", Name);
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Getter", 1) ||
      !Shape(Parsed.Root->Children[0].Children[0], "Variable", 3))
    return std::nullopt;
  const auto &Variable = Parsed.Root->Children[0].Children[0];
  const auto &Owner = Variable.Children[0];
  const auto &Property = Variable.Children[1];
  const auto &Type = Variable.Children[2];
  const auto ValidModule = [](const Node &N) {
    return N.Kind == "Module" && N.Text && !N.Text->empty() && !N.Index &&
           N.Children.empty();
  };
  const auto ValidIdentifier = [](const Node &N) {
    return N.Kind == "Identifier" && N.Text && !N.Text->empty() && !N.Index &&
           N.Children.empty();
  };
  const bool ExtensionOwner =
      Shape(Owner, "Extension", 2) && ValidModule(Owner.Children[0]) &&
      Shape(Owner.Children[1], "Class", 2) &&
      Text(Owner.Children[1].Children[0], "Module", "__C") &&
      ValidIdentifier(Owner.Children[1].Children[1]);
  const bool SwiftClassOwner = Shape(Owner, "Class", 2) &&
                               ValidModule(Owner.Children[0]) &&
                               ValidIdentifier(Owner.Children[1]);
  if ((!ExtensionOwner && !SwiftClassOwner) || Property.Kind != "Identifier" ||
      !Property.Text || Property.Text->empty() || Property.Index ||
      !Property.Children.empty() || !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "BoundGenericEnum", 2))
    return std::nullopt;
  const auto &Optional = Type.Children[0];
  if (!Shape(Optional.Children[0], "Type", 1) ||
      !Nominal(Optional.Children[0].Children[0], "Enum", "Swift", "Optional") ||
      !Shape(Optional.Children[1], "TypeList", 1) ||
      !Shape(Optional.Children[1].Children[0], "Type", 1))
    return std::nullopt;
  const auto &Value = Optional.Children[1].Children[0].Children[0];
  const bool UInt64 = Nominal(Value, "Structure", "Swift", "UInt64");
  const bool ObjCClass =
      Shape(Value, "Class", 2) && Text(Value.Children[0], "Module", "__C") &&
      Value.Children[1].Kind == "Identifier" && Value.Children[1].Text &&
      !Value.Children[1].Text->empty() && !Value.Children[1].Index &&
      Value.Children[1].Children.empty();
  if ((!UInt64 && !ObjCClass) || (SwiftClassOwner && !ObjCClass))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = UInt64 ? NdType::makeStruct({NdType::makeInt(8, false),
                                                 NdType::makeInt(8, false)})
                           : NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// An Objective-C class extension getter for an optional Dictionary<String,
// Int> uses swiftself and returns the dictionary's nullable storage word in
// x0. Match the whole type tree so other collection layouts cannot inherit
// this one-word contract.
inline std::optional<SourceFunctionTypeHint>
swiftMangledObjCOptionalStringIntDictionaryGetterSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  const auto Nominal = [&](const Node &N, llvm::StringRef Kind,
                           llvm::StringRef Module, llvm::StringRef Name) {
    return Shape(N, Kind, 2) && Text(N.Children[0], "Module", Module) &&
           Text(N.Children[1], "Identifier", Name);
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Getter", 1) ||
      !Shape(Parsed.Root->Children[0].Children[0], "Variable", 3))
    return std::nullopt;
  const auto &Variable = Parsed.Root->Children[0].Children[0];
  const auto &Extension = Variable.Children[0];
  const auto &Property = Variable.Children[1];
  const auto &Type = Variable.Children[2];
  if (!Shape(Extension, "Extension", 2) ||
      Extension.Children[0].Kind != "Module" || !Extension.Children[0].Text ||
      Extension.Children[0].Text->empty() || Extension.Children[0].Index ||
      !Extension.Children[0].Children.empty() ||
      !Nominal(Extension.Children[1], "Class", "__C", "NSUserDefaults") ||
      Property.Kind != "Identifier" || !Property.Text ||
      Property.Text->empty() || Property.Index || !Property.Children.empty() ||
      !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "BoundGenericEnum", 2))
    return std::nullopt;
  const auto &Optional = Type.Children[0];
  if (!Shape(Optional.Children[0], "Type", 1) ||
      !Nominal(Optional.Children[0].Children[0], "Enum", "Swift", "Optional") ||
      !Shape(Optional.Children[1], "TypeList", 1) ||
      !Shape(Optional.Children[1].Children[0], "Type", 1) ||
      !Shape(Optional.Children[1].Children[0].Children[0],
             "BoundGenericStructure", 2))
    return std::nullopt;
  const auto &Dictionary = Optional.Children[1].Children[0].Children[0];
  if (!Shape(Dictionary.Children[0], "Type", 1) ||
      !Nominal(Dictionary.Children[0].Children[0], "Structure", "Swift",
               "Dictionary") ||
      !Shape(Dictionary.Children[1], "TypeList", 2))
    return std::nullopt;
  const auto &Entries = Dictionary.Children[1].Children;
  if (!Shape(Entries[0], "Type", 1) ||
      !Nominal(Entries[0].Children[0], "Structure", "Swift", "String") ||
      !Shape(Entries[1], "Type", 1) ||
      !Nominal(Entries[1].Children[0], "Structure", "Swift", "Int"))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// The two-color Swift class initializer passes its UIColor objects in x0/x1
// and its allocated receiver in swiftself. Match the observed specialization
// that converts both arguments from owned to guaranteed, and the complete
// constructor type, before binding x20 as a source parameter.
inline std::optional<SourceFunctionTypeHint>
swiftMangledUIColorPairClassInitializerSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 2) ||
      !Shape(Parsed.Root->Children[0], "FunctionSignatureSpecialization", 4) ||
      !Shape(Parsed.Root->Children[1], "Constructor", 3))
    return std::nullopt;
  const auto &Specialization = Parsed.Root->Children[0];
  if (Specialization.Children[0].Kind != "SpecializationPassID" ||
      Specialization.Children[0].Text ||
      Specialization.Children[0].Index != 4 ||
      !Specialization.Children[0].Children.empty() ||
      !Shape(Specialization.Children[1], "FunctionSignatureSpecializationParam",
             1) ||
      !Shape(Specialization.Children[2], "FunctionSignatureSpecializationParam",
             1) ||
      !Shape(Specialization.Children[3], "FunctionSignatureSpecializationParam",
             0))
    return std::nullopt;
  for (unsigned I : {1U, 2U}) {
    const auto &Kind = Specialization.Children[I].Children[0];
    if (Kind.Kind != "FunctionSignatureSpecializationParamKind" || Kind.Text ||
        Kind.Index != 128 || !Kind.Children.empty())
      return std::nullopt;
  }
  const auto &Constructor = Parsed.Root->Children[1];
  const auto &Class = Constructor.Children[0];
  const auto &Labels = Constructor.Children[1];
  const auto &Type = Constructor.Children[2];
  if (!Shape(Class, "Class", 2) || Class.Children[0].Kind != "Module" ||
      !Class.Children[0].Text || Class.Children[0].Text->empty() ||
      Class.Children[0].Index || !Class.Children[0].Children.empty() ||
      Class.Children[1].Kind != "Identifier" || !Class.Children[1].Text ||
      Class.Children[1].Text->empty() || Class.Children[1].Index ||
      !Class.Children[1].Children.empty() || !Shape(Labels, "LabelList", 2) ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             2) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0].Children[0], "Class", 2))
    return std::nullopt;
  for (const auto &Label : Labels.Children)
    if (Label.Kind != "Identifier" || !Label.Text || Label.Text->empty() ||
        Label.Index || !Label.Children.empty())
      return std::nullopt;
  const auto &Arguments = Type.Children[0].Children[0].Children[0].Children[0];
  for (const auto &Argument : Arguments.Children)
    if (!Shape(Argument, "TupleElement", 1) ||
        !Shape(Argument.Children[0], "Type", 1) ||
        !Shape(Argument.Children[0].Children[0], "Class", 2) ||
        !Text(Argument.Children[0].Children[0].Children[0], "Module", "__C") ||
        !Text(Argument.Children[0].Children[0].Children[1], "Identifier",
              "UIColor"))
      return std::nullopt;
  const auto &Result = Type.Children[0].Children[1].Children[0].Children[0];
  if (!Text(Result.Children[0], "Module", *Class.Children[0].Text) ||
      !Text(Result.Children[1], "Identifier", *Class.Children[1].Text))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"firstColor", NdType::makePtr(NdType::makeVoid())},
                     {"secondColor", NdType::makePtr(NdType::makeVoid())},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[2].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// A class initializer with a CGRect parameter takes four floating lanes and a
// receiver in swiftself. Require the complete nominal/return type tree so an
// ObjC thunk or allocating constructor cannot acquire this entry contract.
inline std::optional<SourceFunctionTypeHint>
swiftMangledCGRectClassInitializerSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  const auto Text = [](const Node &N, llvm::StringRef Kind,
                       llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Constructor", 3))
    return std::nullopt;
  const auto &Constructor = Parsed.Root->Children[0];
  const auto &Class = Constructor.Children[0];
  const auto &Labels = Constructor.Children[1];
  const auto &Type = Constructor.Children[2];
  if (!Shape(Class, "Class", 2) || Class.Children[0].Kind != "Module" ||
      !Class.Children[0].Text || Class.Children[0].Text->empty() ||
      Class.Children[0].Index || !Class.Children[0].Children.empty() ||
      Class.Children[1].Kind != "Identifier" || !Class.Children[1].Text ||
      Class.Children[1].Text->empty() || Class.Children[1].Index ||
      !Class.Children[1].Children.empty() || !Shape(Labels, "LabelList", 1) ||
      !Text(Labels.Children[0], "Identifier", "frame") ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             1) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1))
    return std::nullopt;
  const auto &Argument =
      Type.Children[0].Children[0].Children[0].Children[0].Children[0];
  const auto &Result = Type.Children[0].Children[1].Children[0].Children[0];
  if (!Shape(Argument, "TupleElement", 1) ||
      !Shape(Argument.Children[0], "Type", 1) ||
      !Shape(Argument.Children[0].Children[0], "Structure", 2) ||
      !Text(Argument.Children[0].Children[0].Children[0], "Module", "__C") ||
      !Text(Argument.Children[0].Children[0].Children[1], "Identifier",
            "CGRect") ||
      !Shape(Result, "Class", 2) || Result.Children[0].Kind != "Module" ||
      !Result.Children[0].Text || Result.Children[0].Index ||
      !Result.Children[0].Children.empty() ||
      Result.Children[1].Kind != "Identifier" || !Result.Children[1].Text ||
      Result.Children[1].Index || !Result.Children[1].Children.empty() ||
      *Result.Children[0].Text != *Class.Children[0].Text ||
      *Result.Children[1].Text != *Class.Children[1].Text)
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  const auto Double = NdType::makeFloat(8);
  Hint.Parameters = {
      {"frame", NdType::makeStruct({Double, Double, Double, Double})},
      {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[1].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// Swift class init(coder:) takes NSCoder in x0 and the allocated receiver in
// swiftself, then returns a class pointer (optionally null when failable) in
// x0.
// Match the entire initializing-constructor type. The observed dead-coder
// specialization removes only that argument; its unchanged receiver remains
// in swiftself. Other specializations, ObjC thunks and allocating entries
// cannot borrow either entry contract.
inline std::optional<SourceFunctionTypeHint>
swiftMangledCoderClassInitializerSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Count) {
    return N.Kind == Kind && !N.Text && !N.Index && N.Children.size() == Count;
  };
  const auto Named = [](const Node &N, llvm::StringRef Kind,
                        llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty())
    return std::nullopt;
  bool DeadCoder = false;
  const Node *ConstructorNode = nullptr;
  if (Shape(*Parsed.Root, "Global", 1))
    ConstructorNode = &Parsed.Root->Children[0];
  else if (Shape(*Parsed.Root, "Global", 2)) {
    const auto &Specialization = Parsed.Root->Children[0];
    if (!Shape(Specialization, "FunctionSignatureSpecialization", 3) ||
        Specialization.Children[0].Kind != "SpecializationPassID" ||
        Specialization.Children[0].Text ||
        Specialization.Children[0].Index != 4 ||
        !Specialization.Children[0].Children.empty() ||
        !Shape(Specialization.Children[1],
               "FunctionSignatureSpecializationParam", 1) ||
        !Shape(Specialization.Children[2],
               "FunctionSignatureSpecializationParam", 0))
      return std::nullopt;
    const auto &Kind = Specialization.Children[1].Children[0];
    if (Kind.Kind != "FunctionSignatureSpecializationParamKind" || Kind.Text ||
        Kind.Index != 64 || !Kind.Children.empty())
      return std::nullopt;
    DeadCoder = true;
    ConstructorNode = &Parsed.Root->Children[1];
  }
  if (!ConstructorNode || !Shape(*ConstructorNode, "Constructor", 3))
    return std::nullopt;
  const auto &Constructor = *ConstructorNode;
  const auto &Class = Constructor.Children[0];
  const auto &Labels = Constructor.Children[1];
  const auto &Type = Constructor.Children[2];
  if (!Shape(Class, "Class", 2) || Class.Children[0].Kind != "Module" ||
      !Class.Children[0].Text || Class.Children[0].Text->empty() ||
      Class.Children[0].Index || !Class.Children[0].Children.empty() ||
      Class.Children[1].Kind != "Identifier" || !Class.Children[1].Text ||
      Class.Children[1].Text->empty() || Class.Children[1].Index ||
      !Class.Children[1].Children.empty() || !Shape(Labels, "LabelList", 1) ||
      !Named(Labels.Children[0], "Identifier", "coder") ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             1) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1))
    return std::nullopt;
  const auto &Argument =
      Type.Children[0].Children[0].Children[0].Children[0].Children[0];
  const auto &Result = Type.Children[0].Children[1].Children[0].Children[0];
  if (!Shape(Argument, "TupleElement", 1) ||
      !Shape(Argument.Children[0], "Type", 1) ||
      !Shape(Argument.Children[0].Children[0], "Class", 2) ||
      !Named(Argument.Children[0].Children[0].Children[0], "Module", "__C") ||
      !Named(Argument.Children[0].Children[0].Children[1], "Identifier",
             "NSCoder"))
    return std::nullopt;
  const Node *ResultClass = nullptr;
  if (Shape(Result, "Class", 2))
    ResultClass = &Result;
  else if (Shape(Result, "BoundGenericEnum", 2) &&
           Shape(Result.Children[0], "Type", 1) &&
           Shape(Result.Children[0].Children[0], "Enum", 2) &&
           Named(Result.Children[0].Children[0].Children[0], "Module",
                 "Swift") &&
           Named(Result.Children[0].Children[0].Children[1], "Identifier",
                 "Optional") &&
           Shape(Result.Children[1], "TypeList", 1) &&
           Shape(Result.Children[1].Children[0], "Type", 1) &&
           Shape(Result.Children[1].Children[0].Children[0], "Class", 2))
    ResultClass = &Result.Children[1].Children[0].Children[0];
  if (!ResultClass ||
      !Named(ResultClass->Children[0], "Module", *Class.Children[0].Text) ||
      !Named(ResultClass->Children[1], "Identifier", *Class.Children[1].Text))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  if (!DeadCoder)
    Hint.Parameters.push_back({"coder", NdType::makePtr(NdType::makeVoid())});
  Hint.Parameters.push_back({"self", NdType::makePtr(NdType::makeVoid())});
  Hint.Parameters.back().TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// An initializing Swift class constructor for UIViewController's
// init(nibName:bundle:) receives the optional String in x0/x1, the optional
// NSBundle in x2, and the already allocated receiver in swiftself (x20).
// Require the complete unspecialized constructor type and an exact local
// symbol; the Objective-C thunk and allocating constructor have other ABIs.
inline std::optional<SourceFunctionTypeHint>
swiftMangledNibBundleClassInitializerSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Count) {
    return N.Kind == Kind && !N.Text && !N.Index && N.Children.size() == Count;
  };
  const auto Named = [](const Node &N, llvm::StringRef Kind,
                        llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Constructor", 3))
    return std::nullopt;
  const auto &Constructor = Parsed.Root->Children[0];
  const auto &Class = Constructor.Children[0];
  const auto &Labels = Constructor.Children[1];
  const auto &Type = Constructor.Children[2];
  if (!Shape(Class, "Class", 2) || Class.Children[0].Kind != "Module" ||
      !Class.Children[0].Text || Class.Children[0].Text->empty() ||
      Class.Children[0].Index || !Class.Children[0].Children.empty() ||
      Class.Children[1].Kind != "Identifier" || !Class.Children[1].Text ||
      Class.Children[1].Text->empty() || Class.Children[1].Index ||
      !Class.Children[1].Children.empty() || !Shape(Labels, "LabelList", 2) ||
      !Named(Labels.Children[0], "Identifier", "nibName") ||
      !Named(Labels.Children[1], "Identifier", "bundle") ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             2) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0].Children[0], "Class", 2))
    return std::nullopt;
  const auto &Arguments =
      Type.Children[0].Children[0].Children[0].Children[0].Children;
  const auto Optional = [&](const Node &Element, llvm::StringRef Kind,
                            llvm::StringRef Module,
                            llvm::StringRef Identifier) {
    if (!Shape(Element, "TupleElement", 1) ||
        !Shape(Element.Children[0], "Type", 1))
      return false;
    const auto &Value = Element.Children[0].Children[0];
    return Shape(Value, "BoundGenericEnum", 2) &&
           Shape(Value.Children[0], "Type", 1) &&
           Shape(Value.Children[0].Children[0], "Enum", 2) &&
           Named(Value.Children[0].Children[0].Children[0], "Module",
                 "Swift") &&
           Named(Value.Children[0].Children[0].Children[1], "Identifier",
                 "Optional") &&
           Shape(Value.Children[1], "TypeList", 1) &&
           Shape(Value.Children[1].Children[0], "Type", 1) &&
           Shape(Value.Children[1].Children[0].Children[0], Kind, 2) &&
           Named(Value.Children[1].Children[0].Children[0].Children[0],
                 "Module", Module) &&
           Named(Value.Children[1].Children[0].Children[0].Children[1],
                 "Identifier", Identifier);
  };
  const auto &Result = Type.Children[0].Children[1].Children[0].Children[0];
  if (!Optional(Arguments[0], "Structure", "Swift", "String") ||
      !Optional(Arguments[1], "Class", "__C", "NSBundle") ||
      !Named(Result.Children[0], "Module", *Class.Children[0].Text) ||
      !Named(Result.Children[1], "Identifier", *Class.Children[1].Text))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"nib_name_word_0", NdType::makeInt(8, false)},
                     {"nib_name_word_1", NdType::makePtr(NdType::makeVoid())},
                     {"bundle", NdType::makePtr(NdType::makeVoid())},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[3].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// SwiftPM's static Bundle.module initializer is a context-free closure. Its
// complete mangled type supplies the zero-argument NSBundle result, while the
// current HighIR must independently show no incoming register reads. This is
// only an ABI declaration; the bundle-search body still needs source proof.
inline std::optional<SourceFunctionTypeHint>
swiftMangledBundleModuleClosureSourceABI(
    const BinaryImage &Image, va_t Entry, const HighFunc &Function,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry) || Function.Entry != Entry ||
      Function.SourceTypeHint || !Function.Params.empty() ||
      !Function.ReturnType || Function.ReturnType->Size != 8 ||
      (Function.ReturnType->Kind != NdTypeKind::Ptr &&
       Function.ReturnType->Kind != NdTypeKind::Int))
    return std::nullopt;
  const llvm::StringRef FunctionName(Function.Name);
  if (!FunctionName.starts_with("_$sSo8NSBundleC") ||
      !FunctionName.contains("6module"))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only || Only->Name != Function.Name)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Count) {
    return N.Kind == Kind && !N.Text && !N.Index && N.Children.size() == Count;
  };
  const auto Named = [](const Node &N, llvm::StringRef Kind,
                        llvm::StringRef Value) {
    return N.Kind == Kind && N.Text && *N.Text == Value && !N.Index &&
           N.Children.empty();
  };
  const auto NSBundle = [&](const Node &N) {
    return Shape(N, "Class", 2) && Named(N.Children[0], "Module", "__C") &&
           Named(N.Children[1], "Identifier", "NSBundle");
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "ExplicitClosure", 3))
    return std::nullopt;
  const auto &Closure = Parsed.Root->Children[0];
  const auto &Initializer = Closure.Children[0];
  const auto &Number = Closure.Children[1];
  const auto &Type = Closure.Children[2];
  if (!Shape(Initializer, "Initializer", 1) ||
      !Shape(Initializer.Children[0], "Static", 1) ||
      !Shape(Initializer.Children[0].Children[0], "Variable", 3) ||
      Number.Kind != "Number" || Number.Text || !Number.Index ||
      *Number.Index != 0 || !Number.Children.empty() ||
      !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "NoEscapeFunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             0) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !NSBundle(Type.Children[0].Children[1].Children[0].Children[0]))
    return std::nullopt;
  const auto &Variable = Initializer.Children[0].Children[0];
  const auto &Extension = Variable.Children[0];
  if (!Shape(Extension, "Extension", 2) ||
      Extension.Children[0].Kind != "Module" || !Extension.Children[0].Text ||
      Extension.Children[0].Text->empty() || Extension.Children[0].Index ||
      !Extension.Children[0].Children.empty() ||
      !NSBundle(Extension.Children[1]) ||
      !Named(Variable.Children[1], "Identifier", "module") ||
      !Shape(Variable.Children[2], "Type", 1) ||
      !NSBundle(Variable.Children[2].Children[0]))
    return std::nullopt;

  const auto Flow = analyzeHighSourceFlow(Function, true);
  if (!Flow.Complete || !Flow.Items.empty())
    return std::nullopt;
  bool Undefined = false;
  size_t Budget = 100000;
  walkStmts(Function.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Root) {
      std::vector<const HighExpr *> Pending{Root.get()};
      std::set<const HighExpr *> Seen;
      while (!Pending.empty() && !Undefined) {
        const HighExpr *Expression = Pending.back();
        Pending.pop_back();
        if (!Expression || !Seen.insert(Expression).second)
          continue;
        if (!Budget-- || Expression->Kind == ExprKind::Undef) {
          Undefined = true;
          break;
        }
        for (const auto &Operand : Expression->Operands)
          Pending.push_back(Operand.get());
      }
    });
  });
  if (Undefined)
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// A class initializing constructor (cfc, not its allocating cfC entry) takes
// the already allocated object in swiftself and returns that object. Limit the
// declaration to an exact zero-argument constructor whose result repeats the
// same nominal class; other constructor layouts need separate evidence.
inline std::optional<SourceFunctionTypeHint>
swiftMangledZeroArgClassInitializerSourceABI(
    const BinaryImage &Image, va_t Entry,
    const SwiftFunctionSymbolIndex *SymbolIndex = nullptr) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = uniqueSwiftFunctionSymbol(Image, Entry, SymbolIndex);
  if (!Only)
    return std::nullopt;
  llvm::StringRef Name(Only->Name);
  Name.consume_front("_");
  if (!Name.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = 1024;
  Options.MaxNodes = 128;
  Options.MaxDepth = 24;
  Options.MaxMemoryBytes = 65536;
  Options.MaxOperations = 10000;
  const auto Parsed = llvm::swiftDemangle(Name.str(), Options);
  using Node = llvm::SwiftDemangleNode;
  const auto Shape = [](const Node &N, llvm::StringRef Kind, size_t Children) {
    return N.Kind == Kind && !N.Text && !N.Index &&
           N.Children.size() == Children;
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Constructor", 2))
    return std::nullopt;
  const auto &Constructor = Parsed.Root->Children[0];
  const auto &Class = Constructor.Children[0];
  const auto &Type = Constructor.Children[1];
  if (!Shape(Class, "Class", 2) || Class.Children[0].Kind != "Module" ||
      !Class.Children[0].Text || Class.Children[0].Text->empty() ||
      Class.Children[0].Index || !Class.Children[0].Children.empty() ||
      Class.Children[1].Kind != "Identifier" || !Class.Children[1].Text ||
      Class.Children[1].Text->empty() || Class.Children[1].Index ||
      !Class.Children[1].Children.empty() || !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             0) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0].Children[0], "Class", 2))
    return std::nullopt;
  const auto &ResultClass =
      Type.Children[0].Children[1].Children[0].Children[0];
  if (ResultClass.Children[0].Kind != "Module" ||
      ResultClass.Children[1].Kind != "Identifier" ||
      !ResultClass.Children[0].Text || !ResultClass.Children[1].Text ||
      ResultClass.Children[0].Index || ResultClass.Children[1].Index ||
      !ResultClass.Children[0].Children.empty() ||
      !ResultClass.Children[1].Children.empty() ||
      *ResultClass.Children[0].Text != *Class.Children[0].Text ||
      *ResultClass.Children[1].Text != *Class.Children[1].Text)
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makePtr(NdType::makeVoid());
  Hint.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

} // namespace neverd::sdk

#endif
