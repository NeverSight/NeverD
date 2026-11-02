//===- SwiftMetadata.cpp - Bounded stored-property metadata recovery
//-------===//
#include "neverd/loader/Swift/SwiftMetadata.h"

#include "../ObjC/ObjCRuntimeData.h"

#include "neverd/ir/Limits.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/SwiftDemangle.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace neverd {
namespace {
std::optional<uint64_t> scalarStorageWidth(const llvm::SwiftDemangleNode &Type,
                                           bool Darwin64 = false) {
  if (Type.Kind != "Type" || Type.Text || Type.Index ||
      Type.Children.size() != 1)
    return std::nullopt;
  const auto &Nominal = Type.Children[0];
  if (Nominal.Kind != "Structure" || Nominal.Text || Nominal.Index ||
      Nominal.Children.size() != 2 || Nominal.Children[0].Kind != "Module" ||
      !Nominal.Children[0].Text || Nominal.Children[0].Index ||
      !Nominal.Children[0].Children.empty() ||
      Nominal.Children[1].Kind != "Identifier" || !Nominal.Children[1].Text ||
      Nominal.Children[1].Index || !Nominal.Children[1].Children.empty())
    return std::nullopt;
  const llvm::StringRef Module(*Nominal.Children[0].Text);
  const llvm::StringRef Name(*Nominal.Children[1].Text);
  // The frozen Darwin CGFloat stores one Double on both supported 64-bit
  // targets. Its stable mangling retains CoreGraphics even in SDKs that
  // expose the source declaration from CoreFoundation.
  if (Darwin64 && Module == "CoreGraphics" && Name == "CGFloat")
    return 8;
  if (Module != "Swift")
    return std::nullopt;
  if (Name == "Bool" || Name == "Int8" || Name == "UInt8")
    return 1;
  if (Name == "Int16" || Name == "UInt16")
    return 2;
  if (Name == "Int32" || Name == "UInt32" || Name == "Float")
    return 4;
  if (Name == "Int" || Name == "UInt" || Name == "Int64" || Name == "UInt64" ||
      Name == "Double")
    return 8;
  return std::nullopt;
}
std::optional<uint64_t> staticScalarStorageWidth(llvm::StringRef MangledSymbol,
                                                 bool Darwin64) {
  MangledSymbol.consume_front("_");
  if (!MangledSymbol.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = ::neverd::limits::loader::kSwiftBindingInputBytes;
  Options.MaxNodes = ::neverd::limits::loader::kSwiftBindingNodes;
  Options.MaxDepth = ::neverd::limits::loader::kSwiftBindingDepth;
  Options.MaxMemoryBytes = ::neverd::limits::loader::kSwiftBindingMemoryBytes;
  Options.MaxOperations = ::neverd::limits::loader::kSwiftBindingOperations;
  const auto Parsed = llvm::swiftDemangle(MangledSymbol, Options);
  const auto Shape = [](const llvm::SwiftDemangleNode &Node, const char *Kind,
                        size_t Children) {
    return Node.Kind == Kind && !Node.Text && !Node.Index &&
           Node.Children.size() == Children;
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1))
    return std::nullopt;
  const auto &Static = Parsed.Root->Children[0];
  if (!Shape(Static, "Static", 1))
    return std::nullopt;
  const auto &Variable = Static.Children[0];
  if (!Shape(Variable, "Variable", 3))
    return std::nullopt;
  const auto Named = [](const llvm::SwiftDemangleNode &Node,
                        llvm::StringRef Kind) {
    return Node.Kind == Kind && Node.Text && !Node.Text->empty() &&
           !Node.Index && Node.Children.empty();
  };
  const auto Nominal = [&](const llvm::SwiftDemangleNode &Node) {
    return (Node.Kind == "Class" || Node.Kind == "Structure" ||
            Node.Kind == "Enum") &&
           !Node.Text && !Node.Index && Node.Children.size() == 2 &&
           Named(Node.Children[0], "Module") &&
           Named(Node.Children[1], "Identifier");
  };
  const auto &Context = Variable.Children[0];
  if (!Nominal(Context) &&
      !(Shape(Context, "Extension", 2) &&
        Named(Context.Children[0], "Module") && Nominal(Context.Children[1])))
    return std::nullopt;
  const auto &Property = Variable.Children[1];
  if (!Named(Property, "Identifier"))
    return std::nullopt;
  return scalarStorageWidth(Variable.Children[2], Darwin64);
}
} // namespace

std::optional<uint64_t>
swiftStaticScalarStorageWidth(llvm::StringRef MangledSymbol) {
  return staticScalarStorageWidth(MangledSymbol, false);
}

std::optional<SwiftImmutableScalarStorage>
swiftImmutableScalarStorage(const BinaryImage &Image, va_t Address) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || !Image.MachOTwoLevelNamespace ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) || !Address)
    return std::nullopt;
  const Symbol *Storage = nullptr;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.Addr == Address) {
      if (Storage || Symbol.IsFunc)
        return std::nullopt;
      Storage = &Symbol;
    }
  const auto Width =
      Storage ? staticScalarStorageWidth(Storage->Name, true) : std::nullopt;
  if (!Width || Address % *Width || Address > InvalidVA - *Width ||
      (Storage->Size && Storage->Size != *Width))
    return std::nullopt;
  const auto *Section = Image.getSectionFor(Address);
  if (!Section ||
      (Section->Type & llvm::MachO::SECTION_TYPE) != llvm::MachO::S_REGULAR)
    return std::nullopt;
  // A declaration proves the complete object extent, never the distance to
  // the next symbol. Reject other owners and aliases rather than splitting
  // one object into multiple copied identities.
  for (const auto &Symbol : Image.Symbols) {
    if (&Symbol == Storage)
      continue;
    if (Symbol.Name == Storage->Name ||
        (Symbol.Addr >= Address && Symbol.Addr < Address + *Width) ||
        (Symbol.Size && Symbol.Addr < Address &&
         Address - Symbol.Addr < Symbol.Size))
      return std::nullopt;
  }
  for (const auto &Export : Image.Exports)
    if ((Export.Addr >= Address && Export.Addr < Address + *Width) ||
        Export.Name == Storage->Name)
      if (Export.Addr != Address || Export.Name != Storage->Name)
        return std::nullopt;
  for (uint64_t I = 0; I < *Width; ++I)
    if (Image.hasExecutableCodeOwnerAt(Address + I) ||
        Image.isRuntimeFunctionAt(Address + I))
      return std::nullopt;
  const auto Bytes = readImmutableImageBytes(Image, Address, *Width);
  if (!Bytes)
    return std::nullopt;
  uint64_t Bits = 0;
  for (uint64_t I = 0; I < *Width; ++I)
    Bits |= uint64_t((*Bytes)[I]) << (8 * I);
  if (isImagePointerBitPattern(Image, Bits, *Width))
    return std::nullopt;
  return SwiftImmutableScalarStorage{Storage->Name, uint32_t(*Width)};
}

namespace {
using ReflectionNode = llvm::SwiftDemangleNode;
bool reflectionShape(const ReflectionNode &N, llvm::StringRef Kind,
                     size_t Children) {
  return N.Kind == Kind && !N.Text && !N.Index && N.Children.size() == Children;
}
bool reflectionText(const ReflectionNode &N, llvm::StringRef Kind,
                    llvm::StringRef Text) {
  return N.Kind == Kind && N.Text && *N.Text == Text && !N.Index &&
         N.Children.empty();
}
std::optional<ReflectionNode> reflectionTree(llvm::StringRef Name) {
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = ::neverd::limits::loader::kSwiftIdentityInputBytes;
  Options.MaxNodes = ::neverd::limits::loader::kSwiftIdentityNodes;
  Options.MaxDepth = ::neverd::limits::loader::kSwiftIdentityDepth;
  Options.MaxMemoryBytes = ::neverd::limits::loader::kSwiftIdentityMemoryBytes;
  Options.MaxOperations = ::neverd::limits::loader::kSwiftIdentityOperations;
  auto Parsed = llvm::swiftDemangle(Name, Options);
  return Parsed.Root && Parsed.Error.empty()
             ? std::optional<ReflectionNode>(*Parsed.Root)
             : std::nullopt;
}
// Registered records and fixed metadata are ordinary image storage, never
// thread-local templates. Text may additionally use a C string section.
bool reflectionRecordStorage(const BinaryImage &Image, va_t Address,
                             uint32_t Size, bool Text = false) {
  const auto *Section = Image.getSectionFor(Address);
  if (!Section || Address < Section->VA ||
      Address - Section->VA > Section->Size ||
      Size > Section->Size - (Address - Section->VA))
    return false;
  const auto Kind = Section->Type & llvm::MachO::SECTION_TYPE;
  return Kind == llvm::MachO::S_REGULAR ||
         (Text && Kind == llvm::MachO::S_CSTRING_LITERALS);
}
std::optional<std::vector<uint8_t>>
reflectionRecordBytes(const BinaryImage &Image, va_t Address, uint32_t Size,
                      bool Text = false) {
  return reflectionRecordStorage(Image, Address, Size, Text)
             ? readImmutableImageBytes(Image, Address, Size)
             : std::nullopt;
}
std::optional<va_t> reflectionRelative(const BinaryImage &Image, va_t Address) {
  const auto Bytes = readImmutableImageBytes(Image, Address, 4);
  if (!Bytes)
    return std::nullopt;
  const int64_t Delta = int32_t(llvm::support::endian::read32le(Bytes->data()));
  if (!Delta || (Delta < 0 ? Address < uint64_t(-Delta)
                           : Address > InvalidVA - uint64_t(Delta)))
    return std::nullopt;
  return Delta < 0 ? Address - uint64_t(-Delta) : Address + uint64_t(Delta);
}
std::optional<std::string> reflectionString(const BinaryImage &Image,
                                            va_t Address) {
  const auto Text = objc::RuntimeData(Image).string(Address);
  return Text &&
                 Text->size() <=
                     ::neverd::limits::loader::
                         kSwiftMetadataReflectionStringTextSizeLimit &&
                 readImmutableImageBytes(Image, Address, Text->size() + 1)
             ? Text
             : std::nullopt;
}
bool reflectionClass(const ReflectionNode &N, llvm::StringRef Module,
                     llvm::StringRef Name) {
  return reflectionShape(N, "Class", 2) &&
         reflectionText(N.Children[0], "Module", Module) &&
         reflectionText(N.Children[1], "Identifier", Name);
}
std::optional<std::string> reflectionObjCClass(const BinaryImage &Image,
                                               va_t Address) {
  const auto Encoding = reflectionString(Image, Address);
  if (!Encoding || !llvm::StringRef(*Encoding).starts_with("So"))
    return std::nullopt;
  const auto Tree = reflectionTree("$s" + *Encoding);
  if (!Tree || !reflectionShape(*Tree, "Global", 1) ||
      !reflectionShape(Tree->Children[0], "Class", 2))
    return std::nullopt;
  const auto &N = Tree->Children[0];
  if (!reflectionText(N.Children[0], "Module", "__C") ||
      N.Children[1].Kind != "Identifier" || !N.Children[1].Text ||
      N.Children[1].Text->empty() || N.Children[1].Index ||
      !N.Children[1].Children.empty())
    return std::nullopt;
  return *N.Children[1].Text;
}
std::optional<va_t> reflectionContext(const BinaryImage &Image, va_t Address) {
  const auto Bytes = readImmutableImageBytes(Image, Address, 6);
  if (!Bytes || ((*Bytes)[0] != 1 && (*Bytes)[0] != 2) || (*Bytes)[5])
    return std::nullopt;
  const auto Target = reflectionRelative(Image, Address + 1);
  if (!Target)
    return std::nullopt;
  return (*Bytes)[0] == 1 ? Target : readImmutableImagePointer(Image, *Target);
}
const ObjCClass *reflectionClassRecord(const BinaryImage &Image, va_t Address) {
  const ObjCClass *Result = nullptr;
  for (const auto &C : Image.ObjCClasses)
    if (C.Address == Address) {
      if (Result)
        return nullptr;
      Result = &C;
    }
  if (!Result || llvm::count_if(Image.ObjCClasses, [&](const auto &C) {
                   return C.Name == Result->Name;
                 }) != 1)
    return nullptr;
  return Result;
}
// One declaration owner for fixed-storage queries and runtime-offset receiver
// paths. Reading or freezing an offset remains the individual caller's job.
std::optional<ReflectionNode>
reflectionFieldOffsetType(const BinaryImage &Image,
                          const SwiftObjCClassIdentity &Identity,
                          const ObjCIvar &Ivar) {
  const Symbol *Symbol = nullptr;
  for (const auto &S : Image.Symbols)
    if (S.Addr == Ivar.OffsetAddress) {
      if (Symbol || S.IsFunc || (S.Size && S.Size != 8))
        return std::nullopt;
      Symbol = &S;
    }
  if (!Symbol)
    return std::nullopt;
  for (const auto &S : Image.Symbols)
    if (&S != Symbol && (S.Name == Symbol->Name ||
                         (S.Addr <= Ivar.OffsetAddress
                              ? S.Size && Ivar.OffsetAddress - S.Addr < S.Size
                              : S.Addr - Ivar.OffsetAddress < 8)))
      return std::nullopt;
  for (const auto &Export : Image.Exports)
    if (Export.Addr >= Ivar.OffsetAddress &&
        Export.Addr - Ivar.OffsetAddress < 8 &&
        (Export.Addr != Ivar.OffsetAddress || Export.Name != Symbol->Name))
      return std::nullopt;
  auto Mangled = llvm::StringRef(Symbol->Name);
  Mangled.consume_front("_");
  const auto Tree = reflectionTree(Mangled);
  if (!Tree || !reflectionShape(*Tree, "Global", 1) ||
      !reflectionShape(Tree->Children[0], "FieldOffset", 2))
    return std::nullopt;
  const auto &F = Tree->Children[0];
  if (F.Children[0].Kind != "Directness" || F.Children[0].Text ||
      F.Children[0].Index != 0 || !F.Children[0].Children.empty() ||
      !reflectionShape(F.Children[1], "Variable", 3))
    return std::nullopt;
  const auto &V = F.Children[1];
  const auto &Member = V.Children[1];
  const bool Named =
      reflectionText(Member, "Identifier", Ivar.Name) ||
      (reflectionShape(Member, "PrivateDeclName", 2) &&
       Member.Children[0].Kind == "Identifier" && Member.Children[0].Text &&
       !Member.Children[0].Text->empty() && !Member.Children[0].Index &&
       Member.Children[0].Children.empty() &&
       reflectionText(Member.Children[1], "Identifier", Ivar.Name));
  if (!Named ||
      !reflectionClass(V.Children[0], Identity.Module, Identity.Name) ||
      !reflectionShape(V.Children[2], "Type", 1))
    return std::nullopt;
  return V.Children[2].Children[0];
}

bool reflectionNodesEqual(const ReflectionNode &A, const ReflectionNode &B) {
  if (A.Kind != B.Kind || A.Text != B.Text || A.Index != B.Index ||
      A.Children.size() != B.Children.size())
    return false;
  for (size_t I = 0; I < A.Children.size(); ++I)
    if (!reflectionNodesEqual(A.Children[I], B.Children[I]))
      return false;
  return true;
}
} // namespace

/// The runtime also finds stable internal type names in registered type
/// records. A symbol that is not exported cannot be linked as a descriptor;
/// reconstruct its textual identity only after the descriptor bytes and the
/// image's type or protocol registration agrees with that name. Private
/// anonymous contexts have no such stable lookup identity and remain
/// unsupported. Both record forms reserve the low bits; only direct records
/// without additional flags are accepted here.
/// swift/stdlib/public/runtime/MetadataLookup.cpp:
/// _contextDescriptorMatchesMangling and _searchTypeMetadataRecordsInSections.
static std::optional<std::string>
swiftLocalRegisteredTypeIdentity(const BinaryImage &Image, va_t Address,
                                 bool Protocol) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      !Image.MachOTwoLevelNamespace || Image.MachOChainedFixupsAmbiguous ||
      Address % 4)
    return std::nullopt;
  const Symbol *Descriptor = nullptr;
  for (const auto &Candidate : Image.Symbols)
    if (Candidate.Addr == Address) {
      if (Descriptor || Candidate.IsFunc || Candidate.Name.empty())
        return std::nullopt;
      Descriptor = &Candidate;
    }
  if (!Descriptor || (Descriptor->Size && Descriptor->Size < 12))
    return std::nullopt;
  for (const auto &Candidate : Image.Symbols)
    if (&Candidate != Descriptor && Candidate.Name == Descriptor->Name)
      return std::nullopt;
  for (const auto &Export : Image.Exports)
    if (Export.Addr == Address || Export.Name == Descriptor->Name)
      return std::nullopt;

  llvm::StringRef SymbolName(Descriptor->Name);
  SymbolName.consume_front("_");
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = ::neverd::limits::loader::kSwiftBindingInputBytes;
  Options.MaxNodes = ::neverd::limits::loader::kSwiftBindingNodes;
  Options.MaxDepth = ::neverd::limits::loader::kSwiftBindingDepth;
  Options.MaxMemoryBytes = ::neverd::limits::loader::kSwiftBindingMemoryBytes;
  Options.MaxOperations = ::neverd::limits::loader::kSwiftBindingOperations;
  const auto Parsed = llvm::swiftDemangle(SymbolName, Options);
  const auto Shape = [](const llvm::SwiftDemangleNode &Node,
                        llvm::StringRef Kind, size_t Children) {
    return Node.Kind == Kind && !Node.Text && !Node.Index &&
           Node.Children.size() == Children;
  };
  if (!Parsed.Root || !Parsed.Error.empty() ||
      !Shape(*Parsed.Root, "Global", 1) ||
      !Shape(Parsed.Root->Children[0],
             Protocol ? "ProtocolDescriptor" : "NominalTypeDescriptor", 1) ||
      !Shape(Parsed.Root->Children[0].Children[0], "Type", 1))
    return std::nullopt;
  const auto &Nominal = Parsed.Root->Children[0].Children[0].Children[0];
  if (Protocol && Nominal.Kind != "Protocol")
    return std::nullopt;
  const auto NominalKind = [&](const llvm::SwiftDemangleNode &Node) {
    return Node.Kind == "Class"                  ? 16U
           : Node.Kind == "Structure"            ? 17U
           : Node.Kind == "Enum"                 ? 18U
           : Protocol && Node.Kind == "Protocol" ? 3U
                                                 : 0U;
  };
  const auto Identifier = [](const llvm::SwiftDemangleNode &Node,
                             llvm::StringRef ExpectedKind) {
    return Node.Kind == ExpectedKind && Node.Text && !Node.Index &&
           Node.Children.empty() && !Node.Text->empty() &&
           Node.Text->size() <=
               ::neverd::limits::loader::
                   kSwiftMetadataSwiftLocalRegisteredTypeIdentityIdentifierNodeTextSizeLimit &&
           std::all_of(Node.Text->begin(), Node.Text->end(),
                       [](char C) { return llvm::isAlnum(C) || C == '_'; });
  };
  std::vector<std::pair<unsigned, std::string>> Contexts;
  const auto *Context = &Nominal;
  while (const auto ContextKind = NominalKind(*Context)) {
    if (Contexts.size() == 8 || Context->Text || Context->Index ||
        Context->Children.size() != 2 ||
        !Identifier(Context->Children[1], "Identifier"))
      return std::nullopt;
    Contexts.emplace_back(ContextKind, *Context->Children[1].Text);
    Context = &Context->Children[0];
  }
  const auto &Module = *Context;
  if (Contexts.empty() || !Identifier(Module, "Module") ||
      *Module.Text == "__C" || *Module.Text == "__C_Synthesized" ||
      !SymbolName.starts_with("$s") ||
      !SymbolName.ends_with(Protocol ? "Mp" : "Mn"))
    return std::nullopt;
  const unsigned Kind = Contexts.front().first;

  const auto MatchesText = [&](va_t Target,
                               const std::string &Text) -> std::optional<bool> {
    // Stop at the first different byte. An unrelated short name may be
    // immediately followed by pointer storage that is not part of its text.
    for (size_t I = 0; I <= Text.size(); ++I) {
      if (Target > InvalidVA - I)
        return std::nullopt;
      const auto Byte = reflectionRecordBytes(Image, Target + I, 1, true);
      if (!Byte)
        return std::nullopt;
      const uint8_t Expected = I == Text.size() ? 0 : uint8_t(Text[I]);
      if (Byte->front() != Expected)
        return false;
    }
    return true;
  };
  const auto MatchesIdentity = [&](va_t Target) -> std::optional<bool> {
    std::set<va_t> Seen;
    bool StableContexts = true;
    for (const auto &[ContextKind, ContextName] : Contexts) {
      if (Target % 4 || !Seen.insert(Target).second)
        return std::nullopt;
      const auto Bytes = reflectionRecordBytes(Image, Target, 12);
      if (!Bytes)
        return std::nullopt;
      const uint32_t Flags = llvm::support::endian::read32le(Bytes->data());
      if ((Flags & 0x1f) != ContextKind)
        return false;
      const auto TypeName = reflectionRelative(Image, Target + 8);
      if (!TypeName)
        return std::nullopt;
      const auto NameMatches = MatchesText(*TypeName, ContextName);
      if (!NameMatches || !*NameMatches)
        return NameMatches;
      // Unknown versions, import identities and indirect parents cannot prove
      // which textual declaration the runtime will select.
      if ((Flags & 0xff00) || (Flags & 0x40000) ||
          (llvm::support::endian::read32le(Bytes->data() + 4) & 1))
        return std::nullopt;
      StableContexts &= (Flags & 0xffff) == (ContextKind | 0x40);
      const auto Parent = reflectionRelative(Image, Target + 4);
      if (!Parent)
        return std::nullopt;
      Target = *Parent;
    }
    if (Target % 4 || !Seen.insert(Target).second)
      return std::nullopt;
    const auto ParentBytes = reflectionRecordBytes(Image, Target, 12);
    if (!ParentBytes)
      return std::nullopt;
    const uint32_t ParentFlags =
        llvm::support::endian::read32le(ParentBytes->data());
    if ((ParentFlags & 0x1f) != 0)
      return false;
    if ((ParentFlags & ~uint32_t(0x40)) ||
        llvm::support::endian::read32le(ParentBytes->data() + 4))
      return std::nullopt;
    const auto ModuleName = reflectionRelative(Image, Target + 8);
    const auto ModuleMatches = ModuleName
                                   ? MatchesText(*ModuleName, *Module.Text)
                                   : std::optional<bool>{};
    if (!ModuleMatches || !*ModuleMatches)
      return ModuleMatches;
    return StableContexts ? std::optional<bool>{true} : std::nullopt;
  };
  const auto Header = reflectionRecordBytes(Image, Address, 12);
  const auto Identity = MatchesIdentity(Address);
  if (!Header || !Identity || !*Identity ||
      (llvm::support::endian::read32le(Header->data()) & 0xffff) !=
          (Kind | 0x40))
    return std::nullopt;

  const Section *Records = nullptr;
  for (const auto &Candidate : Image.Sections)
    if (Candidate.Name == (Protocol ? "__swift5_protos" : "__swift5_types")) {
      if (Records)
        return std::nullopt;
      Records = &Candidate;
    }
  if (!Records || !Records->isReadable() || Records->isWritable() ||
      !Records->Size || Records->Size % 4 ||
      Records->Size / 4 >
          ::neverd::limits::loader::
              kSwiftMetadataSwiftLocalRegisteredTypeIdentityRecordsSize4Limit)
    return std::nullopt;
  const auto Bytes = reflectionRecordBytes(Image, Records->VA, Records->Size);
  if (!Bytes)
    return std::nullopt;
  bool Registered = false;
  for (uint64_t I = 0; I < Records->Size; I += 4) {
    const uint32_t Reference =
        llvm::support::endian::read32le(Bytes->data() + I);
    if (!Reference)
      continue;
    if (Reference & 3)
      return std::nullopt;
    const auto Target = reflectionRelative(Image, Records->VA + I);
    const auto Match = Target ? MatchesIdentity(*Target) : std::nullopt;
    if (!Match || (*Match && *Target != Address))
      return std::nullopt;
    Registered |= *Target == Address;
  }
  return Registered ? std::optional<std::string>(
                          SymbolName.drop_front(2).drop_back(2).str() +
                          (Protocol ? "_p" : ""))
                    : std::nullopt;
}

std::optional<std::string>
swiftLocalRegisteredNominalType(const BinaryImage &Image, va_t Address) {
  return swiftLocalRegisteredTypeIdentity(Image, Address, false);
}

std::optional<std::string>
swiftLocalRegisteredProtocolType(const BinaryImage &Image, va_t Address) {
  // A single ordinary Swift protocol has simple existential metadata with
  // one witness table. Exclude requirement signatures, associated types,
  // special protocols and unknown flags before using that representation.
  const auto Header = reflectionRecordBytes(Image, Address, 24);
  if (!Header ||
      (llvm::support::endian::read32le(Header->data()) & ~uint32_t(0x10000)) !=
          0x43 ||
      llvm::support::endian::read32le(Header->data() + 12) ||
      llvm::support::endian::read32le(Header->data() + 20))
    return std::nullopt;
  const uint32_t Requirements =
      llvm::support::endian::read32le(Header->data() + 16);
  if (Requirements > ::neverd::limits::loader::kMaxSwiftProtocolRequirements ||
      !reflectionRecordBytes(Image, Address, 24 + Requirements * 8))
    return std::nullopt;
  return swiftLocalRegisteredTypeIdentity(Image, Address, true);
}

bool swiftImportedCGFloatDescriptor(const BinaryImage &Image, va_t Slot) {
  const auto Bind = Image.DyldBindSlots.find(Slot);
  const auto *Section = Image.getSectionFor(Slot);
  return Image.MachOTwoLevelNamespace && Section &&
         (Section->Type & llvm::MachO::SECTION_TYPE) ==
             llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS &&
         isImmutableImageImportSlot(Image, Slot) &&
         Bind != Image.DyldBindSlots.end() &&
         Bind->second.Name == "_$s12CoreGraphics7CGFloatVMn" &&
         Bind->second.Module == "/usr/lib/swift/libswiftCoreFoundation.dylib";
}

std::optional<SwiftFixedRecordStorage>
swiftFixedRecordStorage(const BinaryImage &Image, va_t Descriptor) {
  if (!objc::RuntimeData(Image).supportsPlainObjectPointers())
    return std::nullopt;
  const auto Name = swiftLocalRegisteredNominalType(Image, Descriptor);
  const auto Header = reflectionRecordBytes(Image, Descriptor, 28);
  const auto Tree = Name ? reflectionTree("$s" + *Name) : std::nullopt;
  if (!Header || !Tree || !reflectionShape(*Tree, "Global", 1) ||
      !reflectionShape(Tree->Children[0], "Structure", 2) ||
      Tree->Children[0].Children[0].Kind != "Module")
    return std::nullopt;
  const auto Word = [&](unsigned Offset) {
    return llvm::support::endian::read32le(Header->data() + Offset);
  };
  // Unique version-zero nongeneric struct, without dynamic metadata
  // initialization, import identity, resilience or unknown descriptor flags.
  if (Word(0) != 0x51 || Word(20) != 4 || Word(24) != 2)
    return std::nullopt;
  const auto Accessor = reflectionRelative(Image, Descriptor + 12);
  if (!Accessor || !Image.hasAuthenticatedFunctionEntryAt(*Accessor) ||
      !readImmutableCodeBytes(Image, *Accessor,
                              Image.Arch == Arch::AArch64 ? 4 : 1))
    return std::nullopt;

  const std::string MetadataName = "_$s" + *Name + "N";
  const Symbol *Metadata = nullptr;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.Name == MetadataName) {
      if (Metadata || Symbol.IsFunc || !Symbol.Addr || Symbol.Addr % 8 ||
          Symbol.Addr < 8 || Symbol.Addr > InvalidVA - 32 ||
          (Symbol.Size && Symbol.Size < 32))
        return std::nullopt;
      Metadata = &Symbol;
    }
  if (!Metadata)
    return std::nullopt;
  for (const auto &Symbol : Image.Symbols)
    if (&Symbol != Metadata && Symbol.Addr == Metadata->Addr)
      return std::nullopt;
  for (const auto &Export : Image.Exports)
    if (Export.Addr == Metadata->Addr || Export.Name == MetadataName)
      return std::nullopt;
  // A second current metadata object for this descriptor is ambiguous even
  // when its symbol uses a different spelling.
  for (const auto &Symbol : Image.Symbols) {
    if (Symbol.Addr == Metadata->Addr || !Image.isDataAddress(Symbol.Addr) ||
        Symbol.Addr > InvalidVA - 16)
      continue;
    const auto OtherKind = reflectionRecordBytes(Image, Symbol.Addr, 8);
    if (OtherKind &&
        llvm::support::endian::read64le(OtherKind->data()) == 0x200 &&
        readImmutableImagePointer(Image, Symbol.Addr + 8) == Descriptor)
      return std::nullopt;
  }
  const auto Kind = reflectionRecordBytes(Image, Metadata->Addr, 8);
  const auto Offsets = reflectionRecordBytes(Image, Metadata->Addr + 16, 16);
  const auto Table = readImmutableImagePointer(Image, Metadata->Addr - 8);
  if (!Kind || llvm::support::endian::read64le(Kind->data()) != 0x200 ||
      readImmutableImagePointer(Image, Metadata->Addr + 8) != Descriptor ||
      !Offsets || !Table || !*Table || *Table % 8 || *Table > InvalidVA - 88 ||
      !reflectionRecordStorage(Image, Metadata->Addr - 8, 8) ||
      !reflectionRecordStorage(Image, Metadata->Addr + 8, 8))
    return std::nullopt;
  for (unsigned I = 0; I != 32; ++I)
    if (Image.hasExecutableCodeOwnerAt(Metadata->Addr + I) ||
        Image.isRuntimeFunctionAt(Metadata->Addr + I))
      return std::nullopt;

  // Fixed storage does not prove the witness implementations. Require every
  // current code pointer without substituting a guessed direct implementation.
  for (unsigned I = 0; I != 8; ++I)
    if (!reflectionRecordStorage(Image, *Table + I * 8, 8) ||
        !readImmutableImageCodePointer(Image, *Table + I * 8))
      return std::nullopt;
  const auto Layout = reflectionRecordBytes(Image, *Table + 64, 24);
  if (!Layout || llvm::support::endian::read64le(Layout->data()) != 32 ||
      llvm::support::endian::read64le(Layout->data() + 8) != 32 ||
      llvm::support::endian::read32le(Layout->data() + 16) != 0x30007 ||
      llvm::support::endian::read32le(Layout->data() + 20) != 0x7fffffff)
    return std::nullopt;
  // 0x30007 declares alignment eight, non-POD, non-inline, complete and
  // bitwise-takable storage. It supplies no copy/destroy or frame permission.
  const auto Fields = reflectionRelative(Image, Descriptor + 16);
  const auto Records =
      Fields ? reflectionRecordBytes(Image, *Fields, 64) : std::nullopt;
  if (!Records || llvm::support::endian::read32le(Records->data() + 4) ||
      llvm::support::endian::read32le(Records->data() + 8) != (12u << 16) ||
      llvm::support::endian::read32le(Records->data() + 12) != 4)
    return std::nullopt;
  const auto ReflectedType = reflectionRelative(Image, *Fields);
  if (!ReflectedType || !reflectionRecordBytes(Image, *ReflectedType, 6) ||
      reflectionContext(Image, *ReflectedType) != Descriptor)
    return std::nullopt;
  SwiftFixedRecordStorage Result;
  Result.MangledType = *Name;
  Result.Descriptor = Descriptor;
  Result.Metadata = Metadata->Addr;
  Result.ValueWitnessTable = *Table;
  Result.Size = 32;
  Result.Alignment = 8;
  std::set<std::string> Names;
  for (unsigned I = 0; I != 4; ++I) {
    const va_t Record = *Fields + 16 + I * 12;
    const auto Flags =
        llvm::support::endian::read32le(Records->data() + 16 + I * 12);
    const auto Offset =
        llvm::support::endian::read32le(Offsets->data() + I * 4);
    const auto Type = reflectionRelative(Image, Record + 4);
    const auto FieldNameAddress = reflectionRelative(Image, Record + 8);
    const auto FieldName = FieldNameAddress
                               ? reflectionString(Image, *FieldNameAddress)
                               : std::nullopt;
    if ((Flags & ~2u) || Offset != I * 8 || !Type || !FieldName ||
        FieldName->empty() ||
        FieldName->size() >
            ::neverd::limits::loader::
                kSwiftMetadataSwiftFixedRecordStorageFieldNameSizeLimit ||
        !reflectionRecordBytes(Image, *FieldNameAddress, FieldName->size() + 1,
                               true) ||
        !(llvm::isAlpha(FieldName->front()) || FieldName->front() == '_') ||
        !llvm::all_of(*FieldName,
                      [](char C) { return llvm::isAlnum(C) || C == '_'; }) ||
        !Names.insert(*FieldName).second)
      return std::nullopt;
    auto &Field = Result.Fields[I];
    Field.Name = *FieldName;
    Field.Offset = Offset;
    Field.IsMutable = Flags & 2;
    const auto First = reflectionRecordBytes(Image, *Type, 1);
    if (!First)
      return std::nullopt;
    if (I < 3 && First->front() == 'S') {
      const auto Encoding = reflectionRecordBytes(Image, *Type, 3);
      if (!Encoding || (*Encoding)[0] != 'S' || (*Encoding)[1] != 'd' ||
          (*Encoding)[2])
        return std::nullopt;
      Field.MangledType = "Sd";
      continue;
    }
    const auto Encoding = reflectionRecordBytes(Image, *Type, 6);
    const auto Target = reflectionRelative(Image, *Type + 1);
    if (!Encoding || !Target || (*Encoding)[5])
      return std::nullopt;
    if (I < 3) {
      if ((*Encoding)[0] != 2 ||
          !swiftImportedCGFloatDescriptor(Image, *Target))
        return std::nullopt;
      Field.MangledType = "12CoreGraphics7CGFloatV";
    } else {
      // Exactly one local class symbolic reference, with no weak, unowned,
      // Optional or composite suffix. Class storage itself remains opaque.
      if ((*Encoding)[0] != 1)
        return std::nullopt;
      const auto ClassName = swiftLocalRegisteredNominalType(Image, *Target);
      const auto ClassTree =
          ClassName ? reflectionTree("$s" + *ClassName) : std::nullopt;
      const auto ClassHeader = reflectionRecordBytes(Image, *Target, 44);
      if (!ClassTree || !ClassHeader ||
          !reflectionShape(*ClassTree, "Global", 1) ||
          !reflectionShape(ClassTree->Children[0], "Class", 2) ||
          (llvm::support::endian::read32le(ClassHeader->data()) & ~0xc0000050u))
        return std::nullopt;
      Field.MangledType = *ClassName;
      Field.Storage = SwiftFixedRecordField::Kind::StrongReference;
    }
  }
  return Result;
}

std::optional<SwiftObjCClassIdentity>
swiftObjCClassIdentity(const BinaryImage &Image, va_t Metadata) {
  objc::RuntimeData Data(Image);
  const auto *Class = reflectionClassRecord(Image, Metadata);
  if (!Class || !Metadata || Metadata % 8 || Metadata > InvalidVA - 4096 ||
      !Data.supportsPlainObjectPointers() || !Data.bytes(Metadata, 80) ||
      Data.className(Metadata) != Class->Name ||
      (readInitialImagePointer(Image, Metadata + 32).value_or(0) & 3) != 2)
    return std::nullopt;
  const auto Descriptor = readInitialImagePointer(Image, Metadata + 64);
  const auto Header = Descriptor
                          ? readImmutableImageBytes(Image, *Descriptor, 44)
                          : std::nullopt;
  if (!Header)
    return std::nullopt;
  const auto Word = [&](unsigned Offset) {
    return llvm::support::endian::read32le(Header->data() + Offset);
  };
  // Version-zero unique non-generic class, no resilient superclass, actor,
  // or dynamic metadata initialization. Vtable/override trailers add no
  // authority to this declaration-only proof.
  if ((Word(0) & ~0xc0000050u) || (Word(0) & 0xffffu) != 0x50u ||
      Word(24) > ::neverd::limits::loader::kMaxSwiftClassMetadataWords ||
      Word(28) > ::neverd::limits::loader::kMaxSwiftClassMetadataWords ||
      Word(36) > ::neverd::limits::loader::kMaxSwiftClassStoredProperties ||
      Word(40) > Word(28) || Word(36) > Word(28) - Word(40) ||
      Data.u32(Metadata + 60) != uint64_t(Word(24)) * 8 ||
      Data.u32(Metadata + 56) != uint64_t(Word(24) + Word(28)) * 8 ||
      (Word(4) & 1))
    return std::nullopt;
  const auto Parent = reflectionRelative(Image, *Descriptor + 4);
  const auto NameAddress = reflectionRelative(Image, *Descriptor + 8);
  const auto ParentBytes =
      Parent ? readImmutableImageBytes(Image, *Parent, 12) : std::nullopt;
  if (!ParentBytes ||
      (llvm::support::endian::read32le(ParentBytes->data()) & ~0x40u) ||
      llvm::support::endian::read32le(ParentBytes->data() + 4))
    return std::nullopt;
  const auto ModuleAddress = reflectionRelative(Image, *Parent + 8);
  const auto Module =
      ModuleAddress ? reflectionString(Image, *ModuleAddress) : std::nullopt;
  const auto Name =
      NameAddress ? reflectionString(Image, *NameAddress) : std::nullopt;
  const auto Tree = reflectionTree(Class->Name);
  if (!Module || !Name || !Tree || !reflectionShape(*Tree, "Global", 1) ||
      !reflectionShape(Tree->Children[0], "TypeMangling", 1) ||
      !reflectionShape(Tree->Children[0].Children[0], "Type", 1) ||
      !reflectionClass(Tree->Children[0].Children[0].Children[0], *Module,
                       *Name))
    return std::nullopt;
  // A native Swift root has no Swift superclass descriptor, but Objective-C
  // records name the runtime's SwiftObject base. Its exact strong import
  // authenticates only this declaration, not a local superclass address or
  // a stable instance/field layout.
  if (!Class->RootClass && !Class->SuperclassAddress &&
      Class->SuperclassName == "_TtCs12_SwiftObject") {
    const auto Bind = Image.DyldBindSlots.find(Metadata + 8);
    constexpr llvm::StringLiteral Provider =
        "/usr/lib/swift/libswiftCore.dylib";
    const auto RO = Data.classRO(Metadata);
    if (Word(20) || Class->InheritanceStatus != "resolved" ||
        !Image.MachOTwoLevelNamespace ||
        !isInitialImageImportSlot(Image, Metadata + 8) ||
        Bind == Image.DyldBindSlots.end() || Bind->second.Module != Provider ||
        Bind->second.Name != "_OBJC_CLASS_$__TtCs12_SwiftObject" ||
        !llvm::is_contained(Image.DynInfo.NeededLibs, Provider) ||
        Class->InstanceStart < 16 ||
        Class->InstanceSize < Class->InstanceStart || !RO ||
        !readInitialImageBytes(Image, *RO, 12) ||
        !readInitialImageBytes(Image, Metadata + 40, 24) ||
        Data.u32(*RO + 4) != Class->InstanceStart ||
        Data.u32(*RO + 8) != Class->InstanceSize ||
        Data.u32(Metadata + 44) != 0 ||
        Data.u32(Metadata + 48) != Class->InstanceSize)
      return std::nullopt;
    return SwiftObjCClassIdentity{*Module, *Name, Class->Name, Metadata,
                                  *Descriptor};
  }
  const auto Super = Class->RootClass
                         ? Data.localPointer(Metadata + 8)
                         : readInitialImagePointer(Image, Metadata + 8);
  if (!Super || *Super != Class->SuperclassAddress)
    return std::nullopt;
  if (Class->RootClass) {
    if (*Super || Class->InheritanceStatus != "root" ||
        !Class->SuperclassName.empty() || Word(20))
      return std::nullopt;
  } else {
    if (!*Super || Class->InheritanceStatus != "resolved" ||
        Class->SuperclassName.empty() ||
        Data.className(*Super) != Class->SuperclassName)
      return std::nullopt;
    const auto SuperType = reflectionRelative(Image, *Descriptor + 20);
    const auto SuperDescriptor = readInitialImagePointer(Image, *Super + 64);
    if (!SuperType || !SuperDescriptor ||
        reflectionContext(Image, *SuperType) != SuperDescriptor)
      return std::nullopt;
  }
  return SwiftObjCClassIdentity{*Module, *Name, Class->Name, Metadata,
                                *Descriptor};
}

std::optional<SwiftFixedRootClassStorage>
swiftFixedRootClassStorage(const BinaryImage &Image, va_t Metadata) {
  const auto Identity = swiftObjCClassIdentity(Image, Metadata);
  const auto *Class = reflectionClassRecord(Image, Metadata);
  if (!Identity || !Class || Class->RootClass || Class->SuperclassAddress ||
      Class->SuperclassName != "_TtCs12_SwiftObject" ||
      Class->IvarStatus != "recovered" || Class->InstanceStart != 16 ||
      Class->InstanceSize < 16 ||
      Class->InstanceSize >
          ::neverd::limits::loader::
              kSwiftMetadataSwiftFixedRootClassStorageClassInstanceSizeLimit ||
      !reflectionRecordStorage(Image, Metadata, 80))
    return std::nullopt;
  const auto Registered =
      swiftLocalRegisteredNominalType(Image, Identity->Descriptor);
  const auto RegisteredTree =
      Registered ? reflectionTree("$s" + *Registered) : std::nullopt;
  if (!RegisteredTree || !reflectionShape(*RegisteredTree, "Global", 1) ||
      !reflectionClass(RegisteredTree->Children[0], Identity->Module,
                       Identity->Name))
    return std::nullopt;
  objc::RuntimeData Data(Image);
  const va_t D = Identity->Descriptor;
  const auto Count = Data.u32(D + 36), Vector = Data.u32(D + 40);
  const auto Fields = reflectionRelative(Image, D + 16);
  const auto RO = Data.classRO(Metadata);
  const auto Ivars =
      RO ? readInitialImagePointer(Image, *RO + 48) : std::nullopt;
  if (!Count || !*Count ||
      *Count > ::neverd::limits::loader::
                   kSwiftMetadataSwiftFixedRootClassStorageCountLimit ||
      !Vector || *Vector != 10 || Data.u32(D + 28).value_or(0) < 10 + *Count ||
      Data.u32(D + 32) != Data.u32(D + 28).value_or(0) - 10 || !Fields || !RO ||
      !Ivars || Class->Ivars.size() != *Count ||
      !reflectionRecordStorage(Image, *RO, 72) ||
      !reflectionRecordStorage(Image, *Ivars, 8 + *Count * 32) ||
      Data.u32(Metadata + 40) != 2 || Data.u32(Metadata + 52) != 7 ||
      !reflectionRecordBytes(Image, *Fields, 16 + *Count * 12) ||
      Data.u32(*Fields + 8) != ((12u << 16) | 1u) ||
      Data.u32(*Fields + 12) != *Count || Data.u32(*Fields + 4) != 0 ||
      !readInitialImageBytes(Image, *Ivars, 8) || Data.u32(*Ivars) != 32 ||
      Data.u32(*Ivars + 4) != *Count)
    return std::nullopt;
  const auto SelfType = reflectionRelative(Image, *Fields);
  const auto Offsets = readInitialImageBytes(
      Image, Metadata + uint64_t(*Vector) * 8, *Count * 8);
  if (!SelfType || reflectionContext(Image, *SelfType) != D || !Offsets)
    return std::nullopt;
  SwiftFixedRootClassStorage Result{*Identity, Class->InstanceSize, 8, {}};
  uint32_t End = 16;
  std::set<std::string> Names;
  std::set<va_t> Slots;
  for (uint32_t I = 0; I < *Count; ++I) {
    const va_t Record = *Fields + 16 + uint64_t(I) * 12;
    const auto &Ivar = Class->Ivars[I];
    const auto NameAddress = reflectionRelative(Image, Record + 8);
    const auto TypeAddress = reflectionRelative(Image, Record + 4);
    const auto Name =
        NameAddress ? reflectionString(Image, *NameAddress) : std::nullopt;
    const auto Type =
        TypeAddress ? reflectionString(Image, *TypeAddress) : std::nullopt;
    if (!Name || !Type || *Name != Ivar.Name ||
        !reflectionRecordStorage(Image, *NameAddress, Name->size() + 1, true) ||
        !reflectionRecordStorage(Image, *TypeAddress, Type->size() + 1, true) ||
        !Names.insert(*Name).second ||
        !Slots.insert(Ivar.OffsetAddress).second ||
        (Data.u32(Record).value_or(~2u) & ~2u))
      return std::nullopt;
    uint32_t Width = 0, Alignment = 0;
    if (*Type == "SSSg" || *Type == "SSSgSg") {
      Width = 16;
      Alignment = 8;
    } else if (*Type == "SbSg") {
      Width = Alignment = 1;
    } else
      return std::nullopt;
    const auto Offset = readImmutableImageIvarOffset(Image, Ivar.OffsetAddress);
    const auto Ref = Image.ObjCSourceReferences.find(Ivar.OffsetAddress);
    const auto RawName =
        readInitialImagePointer(Image, Ivar.MetadataAddress + 8);
    const auto RawType =
        readInitialImagePointer(Image, Ivar.MetadataAddress + 16);
    const auto Declared = reflectionFieldOffsetType(Image, *Identity, Ivar);
    const auto Reflected = reflectionTree("$s" + *Type);
    const uint32_t ExpectedOffset = (End + Alignment - 1) & ~(Alignment - 1);
    if (!Offset || *Offset != ExpectedOffset || !Ivar.Offset ||
        *Ivar.Offset != *Offset || Ivar.Size != Width ||
        Ivar.Alignment != Alignment || !Ivar.TypeEncoding.empty() ||
        Ivar.MetadataAddress != *Ivars + 8 + uint64_t(I) * 32 ||
        readInitialImagePointer(Image, Ivar.MetadataAddress) !=
            Ivar.OffsetAddress ||
        !RawName || reflectionString(Image, *RawName) != Name || !RawType ||
        !readImmutableImageBytes(Image, *RawType, 1) ||
        Data.string(*RawType, true) != std::optional<std::string>("") ||
        !readInitialImageBytes(Image, Ivar.MetadataAddress + 24, 8) ||
        Data.u32(Ivar.MetadataAddress + 24) != (Alignment == 8 ? 3u : 0u) ||
        Data.u32(Ivar.MetadataAddress + 28) != Width ||
        Ref == Image.ObjCSourceReferences.end() || Ref->second.Name != *Name ||
        Ref->second.ClassName != Class->Name ||
        llvm::support::endian::read64le(Offsets->data() + I * 8) != *Offset ||
        !rangeInBounds(*Offset, Width, Result.Size) || !Declared ||
        !Reflected || !reflectionShape(*Reflected, "Global", 1) ||
        !reflectionNodesEqual(*Declared, Reflected->Children[0]))
      return std::nullopt;
    End = ExpectedOffset + Width;
    Result.Fields.push_back({*Name, *Type, ExpectedOffset, Width, Alignment});
  }
  return End == Result.Size ? std::optional(std::move(Result)) : std::nullopt;
}

std::optional<std::string> swiftObjCStoredFieldClass(const BinaryImage &Image,
                                                     llvm::StringRef ClassName,
                                                     va_t OffsetSlot) {
  const ObjCClass *Class = nullptr;
  for (const auto &C : Image.ObjCClasses)
    if (C.Name == ClassName) {
      if (Class)
        return std::nullopt;
      Class = &C;
    }
  const auto Identity =
      Class ? swiftObjCClassIdentity(Image, Class->Address) : std::nullopt;
  if (!Identity || Class->IvarStatus != "recovered" || !OffsetSlot ||
      OffsetSlot % 8)
    return std::nullopt;
  objc::RuntimeData Data(Image);
  const auto D = Identity->Descriptor;
  const auto Count = Data.u32(D + 36), Vector = Data.u32(D + 40);
  const auto Fields = reflectionRelative(Image, D + 16);
  const auto RO = Data.classRO(Class->Address);
  const auto Ivars =
      RO ? readInitialImagePointer(Image, *RO + 48) : std::nullopt;
  if (!Count || !*Count || !Vector || !*Vector || !Fields || !Ivars || !RO ||
      !readInitialImageBytes(Image, *RO, 12) ||
      !readInitialImageBytes(Image, *Ivars, 8) ||
      Data.u32(*RO + 4) != Class->InstanceStart ||
      Data.u32(*RO + 8) != Class->InstanceSize ||
      Class->Ivars.size() != *Count ||
      !readImmutableImageBytes(Image, *Fields, 16 + uint64_t(*Count) * 12) ||
      Data.u32(*Fields + 8) != ((12u << 16) | 7u) ||
      Data.u32(*Fields + 12) != *Count || Data.u32(*Ivars) != 32 ||
      Data.u32(*Ivars + 4) != *Count ||
      !Data.bytes(*Ivars, 8 + uint64_t(*Count) * 32))
    return std::nullopt;
  const auto SelfType = reflectionRelative(Image, *Fields);
  if (!SelfType || reflectionContext(Image, *SelfType) != D ||
      reflectionRelative(Image, *Fields + 4) !=
          reflectionRelative(Image, D + 20))
    return std::nullopt;
  const ObjCIvar *Selected = nullptr;
  std::string Result;
  std::set<std::string> Names;
  std::set<va_t> Slots;
  for (uint32_t I = 0; I < *Count; ++I) {
    const auto &Ivar = Class->Ivars[I];
    const va_t Record = *Fields + 16 + uint64_t(I) * 12;
    const auto NameAddress = reflectionRelative(Image, Record + 8);
    const auto Name =
        NameAddress ? reflectionString(Image, *NameAddress) : std::nullopt;
    const auto TypeAddress = reflectionRelative(Image, Record + 4);
    const auto Type =
        TypeAddress ? reflectionObjCClass(Image, *TypeAddress) : std::nullopt;
    const auto Flags = Data.u32(Record);
    const auto RawName =
        readInitialImagePointer(Image, Ivar.MetadataAddress + 8);
    const auto RawType =
        readInitialImagePointer(Image, Ivar.MetadataAddress + 16);
    const auto Offset = Data.bytes(Ivar.OffsetAddress, 8);
    const auto VectorBytes =
        Data.bytes(Class->Address + uint64_t(*Vector + I) * 8, 8);
    const auto Reference = Image.ObjCSourceReferences.find(Ivar.OffsetAddress);
    if (!Name || *Name != Ivar.Name || !Names.insert(*Name).second ||
        !Slots.insert(Ivar.OffsetAddress).second || !Type || !Flags ||
        (*Flags & ~2u) || !Ivar.TypeEncoding.empty() || Ivar.Size != 8 ||
        Ivar.Alignment != 8 ||
        Ivar.MetadataAddress != *Ivars + 8 + uint64_t(I) * 32 ||
        readInitialImagePointer(Image, Ivar.MetadataAddress) !=
            Ivar.OffsetAddress ||
        !RawName || reflectionString(Image, *RawName) != Ivar.Name ||
        !RawType || !readImmutableImageBytes(Image, *RawType, 1) ||
        !readInitialImageBytes(Image, Ivar.MetadataAddress + 24, 8) ||
        !isFileBackedWritableImageRange(Image, Ivar.OffsetAddress, 8) ||
        Data.string(*RawType, true) != std::optional<std::string>("") ||
        Data.u32(Ivar.MetadataAddress + 24) != 3 ||
        Data.u32(Ivar.MetadataAddress + 28) != 8 || !Offset || !VectorBytes ||
        !Ivar.Offset || *Ivar.Offset < Class->InstanceStart ||
        !rangeInBounds(*Ivar.Offset, 8, Class->InstanceSize) ||
        llvm::support::endian::read64le(Offset) != *Ivar.Offset ||
        llvm::support::endian::read64le(VectorBytes) != *Ivar.Offset ||
        Reference == Image.ObjCSourceReferences.end() ||
        Reference->second.TheKind != ObjCSourceReference::Kind::IvarOffset ||
        Reference->second.Address != Ivar.OffsetAddress ||
        Reference->second.Size != 8 || Reference->second.Name != Ivar.Name ||
        Reference->second.ClassName != Class->Name)
      return std::nullopt;
    const auto DeclaredType = reflectionFieldOffsetType(Image, *Identity, Ivar);
    if (!DeclaredType || !reflectionClass(*DeclaredType, "__C", *Type))
      return std::nullopt;
    if (Ivar.OffsetAddress == OffsetSlot) {
      Selected = &Ivar;
      Result = *Type;
    }
  }
  return Selected ? std::optional<std::string>(Result) : std::nullopt;
}

std::optional<uint64_t>
swiftPrivateScalarStorageWidth(llvm::StringRef MangledSymbol) {
  MangledSymbol.consume_front("_");
  if (!MangledSymbol.starts_with("$s"))
    return std::nullopt;
  llvm::SwiftDemangleOptions Options;
  Options.MaxInputBytes = ::neverd::limits::loader::kSwiftBindingInputBytes;
  Options.MaxNodes = ::neverd::limits::loader::kSwiftBindingNodes;
  Options.MaxDepth = ::neverd::limits::loader::kSwiftBindingDepth;
  Options.MaxMemoryBytes = ::neverd::limits::loader::kSwiftBindingMemoryBytes;
  Options.MaxOperations = ::neverd::limits::loader::kSwiftBindingOperations;
  const auto Parsed = llvm::swiftDemangle(MangledSymbol, Options);
  if (!Parsed.Root || !Parsed.Error.empty() || Parsed.Root->Kind != "Global" ||
      Parsed.Root->Text || Parsed.Root->Index ||
      Parsed.Root->Children.size() != 1)
    return std::nullopt;
  const auto &Variable = Parsed.Root->Children[0];
  if (Variable.Kind != "Variable" || Variable.Text || Variable.Index ||
      Variable.Children.size() != 3)
    return std::nullopt;
  const auto &Module = Variable.Children[0];
  const auto &Private = Variable.Children[1];
  if (Module.Kind != "Module" || !Module.Text || Module.Text->empty() ||
      Module.Index || !Module.Children.empty() ||
      Private.Kind != "PrivateDeclName" || Private.Text || Private.Index ||
      Private.Children.size() != 2)
    return std::nullopt;
  const auto &Identity = Private.Children[0];
  const auto &Name = Private.Children[1];
  if (Identity.Kind != "Identifier" || !Identity.Text || Identity.Index ||
      !Identity.Children.empty() || Identity.Text->size() != 33 ||
      Identity.Text->front() != '_' ||
      !std::all_of(Identity.Text->begin() + 1, Identity.Text->end(),
                   [](unsigned char C) { return std::isxdigit(C); }) ||
      Name.Kind != "Identifier" || !Name.Text || Name.Text->empty() ||
      Name.Index || !Name.Children.empty())
    return std::nullopt;
  return scalarStorageWidth(Variable.Children[2]);
}

namespace {
struct Unsupported : std::runtime_error {
  using std::runtime_error::runtime_error;
};

bool identifier(const std::string &Name) {
  if (Name.empty() ||
      !(std::isalpha(static_cast<unsigned char>(Name[0])) || Name[0] == '_'))
    return false;
  return std::all_of(Name.begin(), Name.end(), [](unsigned char C) {
    return C < 128 && (std::isalnum(C) || C == '_');
  });
}

class Reader {
  const BinaryImage &Image;
  objc::RuntimeData Data;
  ImportStorageSlotCollection Imports;
  std::map<va_t, std::set<va_t>> ClassMetadata;
  std::map<va_t, std::set<va_t>> StructMetadata;

  uint32_t u32(va_t Address) const {
    auto Value = Data.u32(Address);
    if (!Value)
      throw Unsupported(
          "Swift metadata extends beyond readable file-backed bytes");
    return *Value;
  }
  uint64_t u64(va_t Address) const {
    auto Bytes = Data.bytes(Address, 8);
    if (!Bytes)
      throw Unsupported(
          "Swift metadata extends beyond readable file-backed bytes");
    return llvm::support::endian::read64le(Bytes);
  }
  void requirePlainEmptyStructLayout(va_t Metadata) const {
    if (Metadata < 8 || Metadata % 8)
      throw Unsupported("Swift empty struct metadata header is misaligned");
    const va_t Slot = Metadata - 8;
    if (!Data.bytes(Slot, 8))
      throw Unsupported(
          "Swift empty struct value-witness table is unavailable");
    const auto Binding = Image.DyldBindSlots.find(Slot);
    const auto Canonical = Imports.Slots.find(Slot);
    // Model the ABI declared by a strong two-level reference to the standard
    // runtime's empty-tuple witnesses, which are also used for empty structs.
    // Both the provider and canonical binding must belong to this exact slot.
    if (!Imports.Conflicts.count(Slot) && Image.MachOTwoLevelNamespace &&
        Binding != Image.DyldBindSlots.end() &&
        Binding->second.Name == "_$sytWV" && Binding->second.Addend == 0 &&
        Binding->second.Module == "/usr/lib/swift/libswiftCore.dylib" &&
        !Binding->second.WeakImport && Canonical != Imports.Slots.end() &&
        Canonical->second.Name == Binding->second.Name &&
        Canonical->second.Addend == Binding->second.Addend &&
        Canonical->second.Evidence == ImportStorageEvidence::LoaderBind)
      return;
    // An imported slot's on-disk payload is not a local table address, even
    // when its numeric value happens to point at readable bytes. Other or
    // conflicting imports cannot fall through to local table interpretation.
    if (Imports.Conflicts.count(Slot) || Imports.Slots.count(Slot) ||
        Image.ImportStorageSlots.count(Slot) ||
        Image.ImportPtrSlots.count(Slot) || Image.DyldBindSlots.count(Slot))
      throw Unsupported("Swift empty struct value-witness table is external "
                        "or ambiguous");
    const auto Table = Data.pointer(Slot);
    if (!Table || !*Table || *Table % 8 || !Data.bytes(*Table, 88))
      throw Unsupported(
          "Swift empty struct value-witness table is unavailable");
    // The 64-bit ABI places size, stride, flags, and extra-inhabitant count
    // after eight required function pointers. Validate the declared storage
    // and value traits; this does not prove those functions' implementations.
    if (u64(*Table + 64) != 0 || u64(*Table + 72) != 1 ||
        u32(*Table + 80) != 0 || u32(*Table + 84) != 0)
      throw Unsupported("Swift empty struct value-witness layout is not the "
                        "plain empty struct contract");
  }
  va_t relative(va_t Address) const {
    const int64_t Delta = static_cast<int32_t>(u32(Address));
    if (!Delta)
      throw Unsupported("required Swift metadata reference is null");
    if ((Delta < 0 && Address < static_cast<uint64_t>(-Delta)) ||
        (Delta > 0 && Address > InvalidVA - Delta))
      throw Unsupported("Swift metadata relative address overflows");
    return Delta < 0 ? Address - static_cast<uint64_t>(-Delta)
                     : Address + Delta;
  }
  std::string text(va_t Address) const {
    auto Value = Data.string(Address);
    if (!Value || !identifier(*Value))
      throw Unsupported("unsafe or unavailable Swift metadata identifier");
    return *Value;
  }
  SwiftSourceType primitive(std::string Encoding) const {
    while (!Encoding.empty() && Encoding[0] == '_')
      Encoding.erase(0, 1);
    if (Encoding.starts_with("$s"))
      Encoding.erase(0, 2);
    if (Encoding.ends_with("Mn"))
      Encoding.resize(Encoding.size() - 2);
    std::string Name;
    const std::map<std::string, std::string> Short{{"Si", "Int"},
                                                   {"Su", "UInt"},
                                                   {"Sb", "Bool"},
                                                   {"Sf", "Float"},
                                                   {"Sd", "Double"}};
    if (auto It = Short.find(Encoding); It != Short.end())
      Name = It->second;
    else {
      for (const std::string Candidate :
           {"Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32",
            "UInt64"})
        if (Encoding ==
            "s" + std::to_string(Candidate.size()) + Candidate + "V")
          Name = Candidate;
    }
    if (Name.empty())
      throw Unsupported(
          "stored-property type is not an established Swift scalar");
    if (Name == "Bool")
      return {SwiftSourceType::Kind::Boolean, Name, 1, false, nullptr};
    if (Name == "Float" || Name == "Double")
      return {SwiftSourceType::Kind::Floating, Name,
              Name == "Float" ? 32u : 64u, false, nullptr};
    const bool Signed = Name.starts_with("Int");
    const auto Suffix = Name.substr(Signed ? 3 : 4);
    const unsigned Bits =
        Suffix.empty() ? 64 : static_cast<unsigned>(std::stoul(Suffix));
    return {SwiftSourceType::Kind::Integer, Name, Bits, Signed, nullptr};
  }
  SwiftSourceType fieldType(va_t Address) const {
    const uint8_t *First = Data.bytes(Address, 1);
    if (!First)
      throw Unsupported("Swift stored-property type is unavailable");
    if (*First == 2) {
      // A symbolic indirect context reference names an exact loader-bound
      // pointer slot. The binding's identity, not its raw chained payload,
      // establishes a standard-library nominal descriptor.
      if (!Data.bytes(Address, 6) || *Data.bytes(Address + 5, 1) != 0)
        throw Unsupported(
            "composite symbolic Swift field types are unsupported");
      const auto Slot = relative(Address + 1);
      auto It = Imports.Slots.find(Slot);
      if (Imports.Conflicts.count(Slot) || It == Imports.Slots.end() ||
          It->second.Addend != 0)
        throw Unsupported("Swift symbolic field type has no exact imported "
                          "descriptor binding");
      return primitive(It->second.Name);
    }
    if (*First < 0x20)
      throw Unsupported("unsupported Swift symbolic field-type reference kind");
    auto Value = Data.string(Address);
    if (!Value)
      throw Unsupported("invalid Swift field type encoding");
    return primitive(*Value);
  }

  va_t metadata(va_t Descriptor, bool IsClass) const {
    // RuntimeData refuses ambiguous pointer fixups while building the index.
    // Preserve that cause instead of reporting the rejected entry as absent.
    if (Image.MachOChainedFixupsAmbiguous)
      throw Unsupported("Swift type metadata has ambiguous fixups");
    const auto &Index = IsClass ? ClassMetadata : StructMetadata;
    auto It = Index.find(Descriptor);
    if (It == Index.end() || It->second.size() != 1)
      throw Unsupported("Swift type metadata address is absent or ambiguous");
    return *It->second.begin();
  }

  SwiftRecoveredType type(va_t Descriptor) const {
    SwiftRecoveredType Result;
    Result.Descriptor = Descriptor;
    try {
      const uint32_t Flags = u32(Descriptor);
      const unsigned Kind = Flags & 31;
      Result.Kind = Kind == 16   ? "class"
                    : Kind == 17 ? "struct"
                                 : "unsupported";
      if (Result.Kind == "unsupported")
        throw Unsupported(
            "only class and struct storage layouts are supported");
      const bool IsClass = Kind == 16;
      if (!Data.bytes(Descriptor, IsClass ? 44 : 28))
        throw Unsupported("Swift type descriptor is truncated");
      Result.Name = text(relative(Descriptor + 8));
      const uint32_t ParentOffset = u32(Descriptor + 4);
      if (ParentOffset & 1)
        throw Unsupported("indirect Swift parent contexts are unsupported");
      const auto Parent = relative(Descriptor + 4);
      if ((u32(Parent) & 31) != 0)
        throw Unsupported("nested Swift type contexts are unsupported");
      Result.Module = text(relative(Parent + 8));
      if (Flags & 0x80)
        throw Unsupported(
            "generic Swift storage layouts require instantiation metadata");
      if (IsClass &&
          ((Flags & (uint32_t(1) << 29)) || u32(Descriptor + 20) != 0))
        throw Unsupported(
            "Swift inherited or resilient class layouts are unsupported");
      if (IsClass && (Flags & (uint32_t(3) << 23)))
        throw Unsupported("Swift actor storage layout is unsupported");
      if ((Flags >> 16) & 3)
        throw Unsupported("Swift metadata requires dynamic initialization");
      const uint32_t Count = u32(Descriptor + (IsClass ? 36 : 20));
      const uint32_t VectorWords = u32(Descriptor + (IsClass ? 40 : 24));
      if (Count > ::neverd::limits::loader::kSwiftMetadataTypeCountLimit ||
          VectorWords > ::neverd::limits::loader::kMaxSwiftMetadataVectorWords)
        throw Unsupported(
            "Swift field layout exceeds the bounded metadata budget");
      const va_t Fields = relative(Descriptor + 16);
      const uint32_t FieldHeader = u32(Fields + 8);
      if ((FieldHeader & 0xffff) != (IsClass ? 1u : 0u) ||
          (FieldHeader >> 16) != 12 || u32(Fields + 12) != Count ||
          !Data.bytes(Fields, 16 + uint64_t(Count) * 12))
        throw Unsupported(
            "Swift stored-property records disagree with the type descriptor");
      Result.Metadata = metadata(Descriptor, IsClass);
      if (Count && !VectorWords)
        throw Unsupported("Swift field-offset vector is absent");
      if (Result.Metadata > InvalidVA - uint64_t(VectorWords) * 8)
        throw Unsupported("Swift field-offset vector overflows");
      const va_t Vector = Result.Metadata + uint64_t(VectorWords) * 8;
      if (Count && !Data.bytes(Vector, uint64_t(Count) * (IsClass ? 8 : 4)))
        throw Unsupported("Swift field-offset vector is truncated");
      Result.Alignment = 1;
      uint64_t PreviousEnd = IsClass ? 16 : 0;
      std::set<std::string> Names;
      for (uint32_t Index = 0; Index < Count; ++Index) {
        const va_t Record = Fields + 16 + uint64_t(Index) * 12;
        const uint32_t FieldFlags = u32(Record);
        if (FieldFlags & ~uint32_t(2))
          throw Unsupported("unsupported Swift stored-property flags");
        SwiftStorageField Field;
        Field.Name = text(relative(Record + 8));
        if (!Names.insert(Field.Name).second)
          throw Unsupported("duplicate Swift stored-property names");
        Field.Type = fieldType(relative(Record + 4));
        Field.Offset = IsClass ? u64(Vector + uint64_t(Index) * 8)
                               : u32(Vector + uint64_t(Index) * 4);
        Field.IsMutable = (FieldFlags & 2) != 0;
        const uint64_t Size =
            Field.Type.TheKind == SwiftSourceType::Kind::Boolean
                ? 1
                : Field.Type.Bits / 8;
        if (Field.Offset < PreviousEnd || Field.Offset % Size ||
            Field.Offset > ::neverd::limits::ir::kMaxSourceObjectBytes - Size)
          throw Unsupported("Swift scalar fields overlap, misalign, or exceed "
                            "the layout bound");
        // A declaration can reproduce this scalar layout only when the actual
        // vector agrees with the platform's natural scalar placement.
        const uint64_t Expected = (PreviousEnd + Size - 1) & ~(Size - 1);
        if (Field.Offset != Expected)
          throw Unsupported("Swift storage contains an unexplained gap");
        PreviousEnd = Field.Offset + Size;
        Result.Alignment = std::max(Result.Alignment, Size);
        Result.Fields.push_back(std::move(Field));
      }
      Result.Size = PreviousEnd;
      if (!IsClass && Count == 0)
        requirePlainEmptyStructLayout(Result.Metadata);
      if (IsClass) {
        const uint32_t InstanceSize = u32(Result.Metadata + 48);
        const uint32_t AlignmentMask = u32(Result.Metadata + 52) & 0xffff;
        if (u32(Result.Metadata + 44) != 0 || InstanceSize != Result.Size ||
            AlignmentMask + 1 < Result.Alignment || AlignmentMask > 15)
          throw Unsupported("Swift class instance bounds disagree with "
                            "stored-property metadata");
        Result.Alignment = AlignmentMask + 1;
      }
      Result.Status = "recovered";
    } catch (const Unsupported &Error) {
      Result.Reason = Error.what();
    }
    return Result;
  }

public:
  explicit Reader(const BinaryImage &Image)
      : Image(Image), Data(Image), Imports(Image.collectImportStorageSlots()) {
    // Index each symbol once; a large type inventory must not rescan the
    // complete symbol table for every descriptor.
    for (const Symbol &Symbol : Image.Symbols) {
      if (Symbol.IsFunc || !Symbol.Addr || !Image.isDataAddress(Symbol.Addr) ||
          Symbol.Addr > InvalidVA - 72)
        continue;
      if (Data.bytes(Symbol.Addr, 16) && u64(Symbol.Addr) == 0x200) {
        if (auto Descriptor = Data.pointer(Symbol.Addr + 8);
            Descriptor && *Descriptor)
          StructMetadata[*Descriptor].insert(Symbol.Addr);
      }
      if (Data.bytes(Symbol.Addr, 72) && (u32(Symbol.Addr + 40) & 2)) {
        if (auto Descriptor = Data.pointer(Symbol.Addr + 64);
            Descriptor && *Descriptor)
          ClassMetadata[*Descriptor].insert(Symbol.Addr);
      }
    }
  }
  std::vector<SwiftRecoveredType> read() const {
    std::vector<SwiftRecoveredType> Result;
    const Section *Section = Image.getSectionByName("__swift5_types");
    if (!Section)
      return Result;
    if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
        Image.IsRelocatable || Section->Size % 4 ||
        Section->Size / 4 >
            ::neverd::limits::loader::kSwiftMetadataReadSectionSize4Limit ||
        !Data.bytes(Section->VA, Section->Size)) {
      SwiftRecoveredType Error;
      Error.Reason = "unsupported or malformed Swift type-reference section";
      Result.push_back(Error);
      return Result;
    }
    std::set<va_t> Seen;
    for (uint64_t Offset = 0; Offset < Section->Size; Offset += 4) {
      const va_t Slot = Section->VA + Offset;
      try {
        const uint32_t Ref = u32(Slot);
        if (!Ref)
          continue;
        if (Ref & 3)
          throw Unsupported("indirect or Objective-C Swift type-reference "
                            "entries are unsupported");
        const va_t Descriptor = relative(Slot);
        if (!Seen.insert(Descriptor).second)
          continue;
        Result.push_back(type(Descriptor));
      } catch (const Unsupported &Error) {
        SwiftRecoveredType Item;
        Item.Descriptor = Slot;
        Item.Reason = Error.what();
        Result.push_back(std::move(Item));
      }
    }
    return Result;
  }
};
} // namespace
std::vector<SwiftRecoveredType> recoverSwiftTypes(const BinaryImage &Image) {
  return Reader(Image).read();
}
} // namespace neverd
