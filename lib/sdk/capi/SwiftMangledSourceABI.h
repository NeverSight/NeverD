#ifndef NEVERD_SDK_CAPI_SWIFTMANGLEDSOURCEABI_H
#define NEVERD_SDK_CAPI_SWIFTMANGLEDSOURCEABI_H

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Demangle/SwiftDemangle.h"

namespace neverd::sdk {

// A deliberately closed Swift type shape. The compiler lowers this five-value
// global function as nine scalar inputs and a two-word String result. The
// function name and module are irrelevant; the complete mangled type tree,
// exact local function symbol, and a caller's second-result-word use agree.
// This is an ABI declaration only. It does not certify the function body.
inline std::optional<SourceFunctionTypeHint>
swiftMangledStringBundleSourceABI(const BinaryImage &Image, va_t Entry,
                                  bool ObservedSecondResultWord) {
  if (!ObservedSecondResultWord || Image.Format != BinaryFormat::MachO ||
      Image.IsRelocatable || Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      !Image.isCodeAddress(Entry))
    return std::nullopt;
  const Symbol *Only = nullptr;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.Addr == Entry && Symbol.IsFunc) {
      if (Only)
        return std::nullopt;
      Only = &Symbol;
    }
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
  const auto &Function = Parsed.Root->Children[0];
  if (!Shape(Function, "Function", 4) ||
      Function.Children[0].Kind != "Module" || !Function.Children[0].Text ||
      Function.Children[0].Text->empty() || Function.Children[0].Index ||
      !Function.Children[0].Children.empty() ||
      Function.Children[1].Kind != "Identifier" || !Function.Children[1].Text ||
      Function.Children[1].Text->empty() || Function.Children[1].Index ||
      !Function.Children[1].Children.empty() ||
      !Shape(Function.Children[2], "LabelList", 5) ||
      !Shape(Function.Children[3], "Type", 1) ||
      !Shape(Function.Children[3].Children[0], "FunctionType", 2))
    return std::nullopt;
  const auto &Labels = Function.Children[2].Children;
  if (!Shape(Labels[0], "FirstElementMarker", 0))
    return std::nullopt;
  for (size_t I = 1; I < Labels.size(); ++I)
    if (Labels[I].Kind != "Identifier" || !Labels[I].Text ||
        Labels[I].Text->empty() || Labels[I].Index ||
        !Labels[I].Children.empty())
      return std::nullopt;
  const auto &Type = Function.Children[3].Children[0];
  if (!Shape(Type.Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Tuple", 5) ||
      !Shape(Type.Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[1].Children[0], "Type", 1))
    return std::nullopt;
  const auto Nominal = [&](const Node &N, llvm::StringRef Kind,
                           llvm::StringRef Module, llvm::StringRef Identifier) {
    return Shape(N, Kind, 2) && Text(N.Children[0], "Module", Module) &&
           Text(N.Children[1], "Identifier", Identifier);
  };
  const auto String = [&](const Node &N) {
    return Nominal(N, "Structure", "Swift", "String");
  };
  const auto Optional = [&](const Node &N, const auto &Inner) {
    return Shape(N, "BoundGenericEnum", 2) && Shape(N.Children[0], "Type", 1) &&
           Nominal(N.Children[0].Children[0], "Enum", "Swift", "Optional") &&
           Shape(N.Children[1], "TypeList", 1) &&
           Shape(N.Children[1].Children[0], "Type", 1) &&
           Inner(N.Children[1].Children[0].Children[0]);
  };
  const auto &Arguments = Type.Children[0].Children[0].Children[0].Children;
  const auto Argument = [&](size_t Index) -> const Node * {
    const auto &Element = Arguments[Index];
    if (!Shape(Element, "TupleElement", 1) ||
        !Shape(Element.Children[0], "Type", 1))
      return nullptr;
    return &Element.Children[0].Children[0];
  };
  const Node *A0 = Argument(0), *A1 = Argument(1), *A2 = Argument(2),
             *A3 = Argument(3), *A4 = Argument(4);
  if (!A0 || !A1 || !A2 || !A3 || !A4 || !String(*A0) ||
      !Optional(*A1, String) ||
      !Optional(*A2,
                [&](const Node &N) {
                  return Nominal(N, "Class", "__C", "NSBundle");
                }) ||
      !String(*A3) || !String(*A4) ||
      !String(Type.Children[1].Children[0].Children[0]))
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeStruct(
      {NdType::makeInt(8, false), NdType::makePtr(NdType::makeVoid())});
  for (unsigned I = 0; I < 9; ++I)
    Hint.Parameters.push_back(
        {"arg" + std::to_string(I), I == 1 || I == 6 || I == 8
                                        ? NdType::makePtr(NdType::makeVoid())
                                        : NdType::makeInt(8, false)});
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

} // namespace neverd::sdk

#endif
