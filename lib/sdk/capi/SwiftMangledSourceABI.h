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

// A Swift extension Bool getter or zero-argument method on an Objective-C
// class carries its receiver in swiftself. The closed mangled type supplies
// the return width; the source pipeline must still prove the body and every
// call before publication.
inline std::optional<SourceFunctionTypeHint>
swiftMangledObjCBoolMemberSourceABI(const BinaryImage &Image, va_t Entry) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
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
  const auto &Root = Parsed.Root->Children[0];
  const Node *Member = nullptr, *Result = nullptr;
  if (Shape(Root, "Getter", 1) &&
      Shape(Root.Children[0], "Variable", 3)) {
    Member = &Root.Children[0];
    if (Shape(Member->Children[2], "Type", 1))
      Result = &Member->Children[2].Children[0];
  } else if (Shape(Root, "Function", 3)) {
    Member = &Root;
    const auto &Type = Root.Children[2];
    if (Shape(Type, "Type", 1) &&
        Shape(Type.Children[0], "FunctionType", 2) &&
        Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) &&
        Shape(Type.Children[0].Children[0].Children[0], "Type", 1) &&
        Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple", 0) &&
        Shape(Type.Children[0].Children[1], "ReturnType", 1) &&
        Shape(Type.Children[0].Children[1].Children[0], "Type", 1))
      Result = &Type.Children[0].Children[1].Children[0].Children[0];
  }
  if (!Member || !Result ||
      !Shape(Member->Children[0], "Extension", 2) ||
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
      Member->Children[1].Kind != "Identifier" ||
      !Member->Children[1].Text || Member->Children[1].Text->empty() ||
      Member->Children[1].Index || !Member->Children[1].Children.empty() ||
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
swiftMangledObjCObjectVoidMethodSourceABI(const BinaryImage &Image,
                                          va_t Entry) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
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
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Function", 4))
    return std::nullopt;
  const auto &Function = Parsed.Root->Children[0];
  const auto &Owner = Function.Children[0];
  const auto &Labels = Function.Children[2];
  const auto &Type = Function.Children[3];
  if (!Shape(Owner, "Class", 2) || Owner.Children[0].Kind != "Module" ||
      !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
      Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
      Owner.Children[1].Kind != "Identifier" || !Owner.Children[1].Text ||
      Owner.Children[1].Text->empty() || Owner.Children[1].Index ||
      !Owner.Children[1].Children.empty() ||
      Function.Children[1].Kind != "Identifier" || !Function.Children[1].Text ||
      Function.Children[1].Text->empty() || Function.Children[1].Index ||
      !Function.Children[1].Children.empty() ||
      !Shape(Labels, "LabelList", 0) || !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Class",
             2) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0].Children[0], "Tuple", 0))
    return std::nullopt;
  const auto &Argument = Type.Children[0].Children[0].Children[0].Children[0];
  const auto &ArgumentName = Argument.Children[1];
  if (!Text(Argument.Children[0], "Module", "__C") ||
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

// An instance method with two Objective-C object arguments and no result uses
// x0/x1 for the objects and swiftself (x20) for its receiver on arm64. Accept
// only the complete mangled type tree; native body and callers remain subject
// to the ordinary source proof.
inline std::optional<SourceFunctionTypeHint>
swiftMangledObjCObjectPairVoidMethodSourceABI(const BinaryImage &Image,
                                              va_t Entry) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
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
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Function", 4))
    return std::nullopt;
  const auto &Function = Parsed.Root->Children[0];
  const auto &Owner = Function.Children[0];
  const auto &Labels = Function.Children[2];
  const auto &Type = Function.Children[3];
  if (!Shape(Owner, "Class", 2) || Owner.Children[0].Kind != "Module" ||
      !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
      Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
      Owner.Children[1].Kind != "Identifier" || !Owner.Children[1].Text ||
      Owner.Children[1].Text->empty() || Owner.Children[1].Index ||
      !Owner.Children[1].Children.empty() ||
      Function.Children[1].Kind != "Identifier" || !Function.Children[1].Text ||
      Function.Children[1].Text->empty() || Function.Children[1].Index ||
      !Function.Children[1].Children.empty() ||
      !Shape(Labels, "LabelList", 2) ||
      !Shape(Labels.Children[0], "FirstElementMarker", 0) ||
      Labels.Children[1].Kind != "Identifier" || !Labels.Children[1].Text ||
      Labels.Children[1].Text->empty() || Labels.Children[1].Index ||
      !Labels.Children[1].Children.empty() || !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             2) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0].Children[0], "Tuple", 0))
    return std::nullopt;
  for (const auto &Argument :
       Type.Children[0].Children[0].Children[0].Children[0].Children) {
    if (!Shape(Argument, "TupleElement", 1) ||
        !Shape(Argument.Children[0], "Type", 1) ||
        !Shape(Argument.Children[0].Children[0], "Class", 2) ||
        !Text(Argument.Children[0].Children[0].Children[0], "Module", "__C") ||
        Argument.Children[0].Children[0].Children[1].Kind != "Identifier" ||
        !Argument.Children[0].Children[0].Children[1].Text ||
        Argument.Children[0].Children[0].Children[1].Text->empty() ||
        Argument.Children[0].Children[0].Children[1].Index ||
        !Argument.Children[0].Children[0].Children[1].Children.empty())
      return std::nullopt;
  }

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeVoid();
  Hint.Parameters = {{"first", NdType::makePtr(NdType::makeVoid())},
                     {"second", NdType::makePtr(NdType::makeVoid())},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[2].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// The UIColor extension allocator taking Int and CGFloat has one integer,
// one double, and the UIColor class metadata in swiftself. A compiler probe
// for the corresponding NSColor extension confirms the mixed register layout.
inline std::optional<SourceFunctionTypeHint>
swiftMangledUIColorIntAlphaAllocatorSourceABI(const BinaryImage &Image,
                                              va_t Entry) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
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
inline std::optional<SourceFunctionTypeHint>
swiftMangledZeroArgClassMethodSourceABI(const BinaryImage &Image, va_t Entry) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
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
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Function", 3))
    return std::nullopt;
  const auto &Function = Parsed.Root->Children[0];
  const auto &Owner = Function.Children[0];
  const auto &Type = Function.Children[2];
  const auto Identifier = [](const Node &N) {
    return N.Kind == "Identifier" && N.Text && !N.Text->empty() && !N.Index &&
           N.Children.empty();
  };
  const auto &Member = Function.Children[1];
  const bool NamedMember =
      Identifier(Member) ||
      (Shape(Member, "PrivateDeclName", 2) && Identifier(Member.Children[0]) &&
       Identifier(Member.Children[1]));
  if (!Shape(Owner, "Class", 2) || Owner.Children[0].Kind != "Module" ||
      !Owner.Children[0].Text || Owner.Children[0].Text->empty() ||
      Owner.Children[0].Index || !Owner.Children[0].Children.empty() ||
      Owner.Children[1].Kind != "Identifier" || !Owner.Children[1].Text ||
      Owner.Children[1].Text->empty() || Owner.Children[1].Index ||
      !Owner.Children[1].Children.empty() || !NamedMember ||
      !Shape(Type, "Type", 1) || !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple",
             0) ||
      !Shape(Type.Children[0].Children[1], "ReturnType", 1) ||
      !Shape(Type.Children[0].Children[1].Children[0], "Type", 1))
    return std::nullopt;
  const auto &Result = Type.Children[0].Children[1].Children[0].Children[0];
  const bool VoidResult = Shape(Result, "Tuple", 0);
  const bool BoolResult = Shape(Result, "Structure", 2) &&
                          Text(Result.Children[0], "Module", "Swift") &&
                          Text(Result.Children[1], "Identifier", "Bool");
  if (!VoidResult && !BoolResult)
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = VoidResult ? NdType::makeVoid() : NdType::makeInt(1, false);
  Hint.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  std::string Error;
  return assignDarwinSwiftSourceABI(Hint, Image.Arch, Error)
             ? std::optional<SourceFunctionTypeHint>(std::move(Hint))
             : std::nullopt;
}

// A Swift class Bool or Int property setter takes its new value in x0 and the
// instance in swiftself. Its mangled Setter/Variable tree distinguishes the
// void result from the Bool property type; generic register-result inference
// must not treat a clobbered x0 as a setter return value.
inline std::optional<SourceFunctionTypeHint>
swiftMangledClassScalarSetterSourceABI(const BinaryImage &Image, va_t Entry) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
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
      !Shape(Type.Children[0], "Structure", 2) ||
      !Text(Type.Children[0].Children[0], "Module", "Swift"))
    return std::nullopt;
  const bool IsBool = Text(Type.Children[0].Children[1], "Identifier", "Bool");
  const bool IsInt = Text(Type.Children[0].Children[1], "Identifier", "Int");
  if (!IsBool && !IsInt)
    return std::nullopt;

  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeVoid();
  Hint.Parameters = {{"value", NdType::makeInt(IsBool ? 1 : 8, !IsBool)},
                     {"self", NdType::makePtr(NdType::makeVoid())}};
  Hint.Parameters[1].TheRole = SourceParameterTypeHint::Role::SwiftContext;
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
swiftMangledZeroArgClassInitializerSourceABI(const BinaryImage &Image,
                                              va_t Entry) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
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
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0], "Constructor", 2))
    return std::nullopt;
  const auto &Constructor = Parsed.Root->Children[0];
  const auto &Class = Constructor.Children[0];
  const auto &Type = Constructor.Children[1];
  if (!Shape(Class, "Class", 2) ||
      Class.Children[0].Kind != "Module" ||
      !Class.Children[0].Text || Class.Children[0].Text->empty() ||
      Class.Children[0].Index || !Class.Children[0].Children.empty() ||
      Class.Children[1].Kind != "Identifier" ||
      !Class.Children[1].Text || Class.Children[1].Text->empty() ||
      Class.Children[1].Index || !Class.Children[1].Children.empty() ||
      !Shape(Type, "Type", 1) ||
      !Shape(Type.Children[0], "FunctionType", 2) ||
      !Shape(Type.Children[0].Children[0], "ArgumentTuple", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0], "Type", 1) ||
      !Shape(Type.Children[0].Children[0].Children[0].Children[0], "Tuple", 0) ||
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
