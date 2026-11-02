//===- PDBTypeImport.cpp - Local types from a PDB's type records ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/PDBTypeImport.h"

#include "neverd/types/CodeViewCallingConvention.h"
#include "neverd/types/Limits.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/CodeView/CVTypeVisitor.h"
#include "llvm/DebugInfo/CodeView/LazyRandomTypeCollection.h"
#include "llvm/DebugInfo/CodeView/TypeDeserializer.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/CodeView/TypeVisitorCallbacks.h"
#include "llvm/DebugInfo/PDB/IPDBSession.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/DebugInfo/PDB/PDBTypes.h"

#include <optional>
#include <string>

namespace neverd::types {
namespace {

namespace cv = llvm::codeview;

/// A record MSVC names only by its place: an anonymous union, structure or
/// enumeration, which the member that uses it defines in place.
bool isUnnamed(llvm::StringRef Name) {
  return Name.empty() || Name.starts_with("<unnamed-") ||
         Name.starts_with("<anonymous-") || Name.starts_with("__unnamed") ||
         Name.contains("::<unnamed-");
}

std::optional<Builtin> builtinOf(cv::SimpleTypeKind Kind) {
  switch (Kind) {
#define NEVERD_PDB_SIMPLE_TYPE(CodeViewKind, BuiltinName)                      \
  case cv::SimpleTypeKind::CodeViewKind:                                       \
    return Builtin::BuiltinName;
#include "PDBSimpleTypes.def"
  default:
    return std::nullopt;
  }
}

/// The size of a pointer a simple type mode makes; zero for a direct type.
unsigned simplePointerBytes(cv::SimpleTypeMode Mode) {
  switch (Mode) {
  case cv::SimpleTypeMode::Direct:
    return 0;
  case cv::SimpleTypeMode::NearPointer:
    return 2;
  case cv::SimpleTypeMode::FarPointer:
  case cv::SimpleTypeMode::HugePointer:
  case cv::SimpleTypeMode::NearPointer32:
  case cv::SimpleTypeMode::FarPointer32:
    return 4;
  case cv::SimpleTypeMode::NearPointer64:
    return 8;
  case cv::SimpleTypeMode::NearPointer128:
    return 16;
  }
  return 8;
}

DataModel dataModelOf(llvm::pdb::PDB_Machine Machine) {
  switch (Machine) {
  case llvm::pdb::PDB_Machine::x86:
  case llvm::pdb::PDB_Machine::ArmNT:
  case llvm::pdb::PDB_Machine::Arm:
  case llvm::pdb::PDB_Machine::Thumb:
    return DataModel::ILP32;
  default:
    return DataModel::LLP64;
  }
}

template <typename RecordT>
std::optional<RecordT> deserialize(const cv::CVType &Type) {
  RecordT Record(static_cast<cv::TypeRecordKind>(Type.kind()));
  if (auto Err = cv::TypeDeserializer::deserializeAs<RecordT>(
          const_cast<cv::CVType &>(Type), Record)) {
    llvm::consumeError(std::move(Err));
    return std::nullopt;
  }
  return Record;
}

bool isClassKind(cv::TypeLeafKind Kind) {
  return Kind == cv::LF_STRUCTURE || Kind == cv::LF_CLASS ||
         Kind == cv::LF_INTERFACE;
}

/// What one field list holds, in order.
class FieldCollector final : public cv::TypeVisitorCallbacks {
public:
  struct Data {
    std::string Name;
    cv::TypeIndex Type;
    uint64_t Offset = 0;
    bool Base = false;
  };

  llvm::Error visitKnownMember(cv::CVMemberRecord &,
                               cv::DataMemberRecord &Record) override {
    Fields.push_back(
        {Record.getName().str(), Record.getType(), Record.getFieldOffset()});
    return llvm::Error::success();
  }
  llvm::Error visitKnownMember(cv::CVMemberRecord &,
                               cv::BaseClassRecord &Record) override {
    Fields.push_back({"baseclass_" + std::to_string(Record.getBaseOffset()),
                      Record.getBaseType(), Record.getBaseOffset(), true});
    return llvm::Error::success();
  }
  llvm::Error visitKnownMember(cv::CVMemberRecord &,
                               cv::VFPtrRecord &Record) override {
    Fields.push_back({"__vftable", Record.getType(), 0});
    return llvm::Error::success();
  }
  llvm::Error visitKnownMember(cv::CVMemberRecord &,
                               cv::EnumeratorRecord &Record) override {
    const llvm::APSInt &Value = Record.getValue();
    Enumerators.push_back({Record.getName().str(),
                           Value.isSigned()
                               ? static_cast<uint64_t>(Value.getSExtValue())
                               : Value.getZExtValue(),
                           std::string()});
    return llvm::Error::success();
  }
  llvm::Error visitKnownMember(cv::CVMemberRecord &,
                               cv::ListContinuationRecord &Record) override {
    Continuation = Record.getContinuationIndex();
    return llvm::Error::success();
  }

  std::vector<Data> Fields;
  std::vector<Enumerator> Enumerators;
  std::optional<cv::TypeIndex> Continuation;
};

class Importer {
public:
  Importer(llvm::pdb::TpiStream &Tpi, TypeLibrary &Library)
      : Tpi(Tpi), Types(Tpi.typeCollection()), Library(Library) {}

  PDBTypeImportStats run();

private:
  /// The type index of the definition a record names: itself when it is
  /// one, the definition of its unique name or name otherwise.
  std::optional<cv::TypeIndex> definition(cv::TypeIndex Index);
  TypeExprRef expr(cv::TypeIndex Index);
  TypeExprRef exprUncached(cv::TypeIndex Index);
  uint64_t sizeOf(cv::TypeIndex Index);
  /// How MSVC aligns a value of \p Index: naturally, but no further than
  /// the #pragma pack of the record it is.
  uint32_t alignOf(cv::TypeIndex Index);
  /// How a record was laid out, found from the offsets and size the
  /// compiler chose.
  struct RecordLayout {
    /// The #pragma pack: zero when its members sit where they would unpacked,
    /// else the largest pack that puts them where they are.
    uint32_t Pack = 0;
    /// The __declspec(align) a size past its members' alignment asks for:
    /// the smallest that pads them to that size; zero for none.
    uint32_t Declared = 0;
    /// How the record aligns.
    uint32_t Alignment = 1;
  };
  const RecordLayout &layoutOf(cv::TypeIndex Definition);
  std::shared_ptr<const RecordDef> record(cv::TypeIndex Definition);
  std::shared_ptr<const EnumDef> enumeration(cv::TypeIndex Definition);
  std::shared_ptr<const FunctionDef> function(cv::TypeIndex Return,
                                              cv::CallingConvention Convention,
                                              cv::TypeIndex Arguments);
  /// The members of the field list \p List, its continuations included.
  FieldCollector fields(cv::TypeIndex List);

  llvm::pdb::TpiStream &Tpi;
  cv::LazyRandomTypeCollection &Types;
  TypeLibrary &Library;
  llvm::StringMap<cv::TypeIndex> ByUniqueName;
  llvm::StringMap<cv::TypeIndex> ByName;
  llvm::DenseMap<uint32_t, TypeExprRef> Exprs;
  llvm::DenseMap<uint32_t, uint64_t> Sizes;
  llvm::DenseMap<uint32_t, uint32_t> Alignments;
  llvm::DenseMap<uint32_t, RecordLayout> Layouts;
  /// Records, named or in place, whose nesting did not flatten back.
  unsigned Mismatches = 0;
  unsigned UnsupportedFunctionTypes = 0;
};

std::optional<cv::TypeIndex> Importer::definition(cv::TypeIndex Index) {
  const cv::CVType Type = Types.getType(Index);
  llvm::StringRef Name, Unique;
  bool Forward = false;
  if (isClassKind(Type.kind())) {
    auto Record = deserialize<cv::ClassRecord>(Type);
    if (!Record)
      return std::nullopt;
    Forward = Record->isForwardRef();
    Name = Record->getName();
    Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
  } else if (Type.kind() == cv::LF_UNION) {
    auto Record = deserialize<cv::UnionRecord>(Type);
    if (!Record)
      return std::nullopt;
    Forward = Record->isForwardRef();
    Name = Record->getName();
    Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
  } else if (Type.kind() == cv::LF_ENUM) {
    auto Record = deserialize<cv::EnumRecord>(Type);
    if (!Record)
      return std::nullopt;
    Forward = Record->isForwardRef();
    Name = Record->getName();
    Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
  } else {
    return std::nullopt;
  }
  if (!Forward)
    return Index;
  if (!Unique.empty())
    if (auto Found = ByUniqueName.find(Unique); Found != ByUniqueName.end())
      return Found->second;
  if (auto Found = ByName.find(Name); Found != ByName.end())
    return Found->second;
  return std::nullopt;
}

uint64_t Importer::sizeOf(cv::TypeIndex Index) {
  if (Index.isNoneType())
    return 0;
  if (Index.isSimple()) {
    if (unsigned Bytes = simplePointerBytes(Index.getSimpleMode()))
      return Bytes;
    auto Kind = builtinOf(Index.getSimpleKind());
    return Kind ? Library.builtinBytes(*Kind) : 0;
  }
  if (auto Found = Sizes.find(Index.getIndex()); Found != Sizes.end())
    return Found->second;
  uint64_t Size = 0;
  const cv::CVType Type = Types.getType(Index);
  switch (Type.kind()) {
  case cv::LF_POINTER:
    if (auto Record = deserialize<cv::PointerRecord>(Type))
      Size = Record->getSize();
    break;
  case cv::LF_MODIFIER:
    if (auto Record = deserialize<cv::ModifierRecord>(Type))
      Size = sizeOf(Record->getModifiedType());
    break;
  case cv::LF_ARRAY:
    if (auto Record = deserialize<cv::ArrayRecord>(Type))
      Size = Record->getSize();
    break;
  case cv::LF_BITFIELD:
    if (auto Record = deserialize<cv::BitFieldRecord>(Type))
      Size = sizeOf(Record->getType());
    break;
  case cv::LF_STRUCTURE:
  case cv::LF_CLASS:
  case cv::LF_INTERFACE:
    if (auto Definition = definition(Index))
      if (auto Record =
              deserialize<cv::ClassRecord>(Types.getType(*Definition)))
        Size = Record->getSize();
    break;
  case cv::LF_UNION:
    if (auto Definition = definition(Index))
      if (auto Record =
              deserialize<cv::UnionRecord>(Types.getType(*Definition)))
        Size = Record->getSize();
    break;
  case cv::LF_ENUM:
    if (auto Record = deserialize<cv::EnumRecord>(Type))
      Size = sizeOf(Record->getUnderlyingType());
    break;
  default:
    break;
  }
  Sizes[Index.getIndex()] = Size;
  return Size;
}

uint32_t Importer::alignOf(cv::TypeIndex Index) {
  if (Index.isNoneType())
    return 1;
  if (Index.isSimple())
    return Library.alignOf(*expr(Index));
  if (auto Found = Alignments.find(Index.getIndex()); Found != Alignments.end())
    return Found->second;
  uint32_t Alignment = 1;
  const cv::CVType Type = Types.getType(Index);
  switch (Type.kind()) {
  case cv::LF_POINTER:
    Alignment = static_cast<uint32_t>(std::max<uint64_t>(sizeOf(Index), 1));
    break;
  case cv::LF_MODIFIER:
    if (auto Record = deserialize<cv::ModifierRecord>(Type))
      Alignment = alignOf(Record->getModifiedType());
    break;
  case cv::LF_ARRAY:
    if (auto Record = deserialize<cv::ArrayRecord>(Type))
      Alignment = alignOf(Record->getElementType());
    break;
  case cv::LF_BITFIELD:
    if (auto Record = deserialize<cv::BitFieldRecord>(Type))
      Alignment = alignOf(Record->getType());
    break;
  case cv::LF_ENUM:
    if (auto Record = deserialize<cv::EnumRecord>(Type))
      Alignment = alignOf(Record->getUnderlyingType());
    break;
  case cv::LF_STRUCTURE:
  case cv::LF_CLASS:
  case cv::LF_INTERFACE:
  case cv::LF_UNION:
    if (auto Definition = definition(Index))
      Alignment = layoutOf(*Definition).Alignment;
    break;
  default:
    break;
  }
  Alignments[Index.getIndex()] = Alignment;
  return Alignment;
}

const Importer::RecordLayout &Importer::layoutOf(cv::TypeIndex Definition) {
  if (auto Found = Layouts.find(Definition.getIndex()); Found != Layouts.end())
    return Found->second;
  const cv::CVType Type = Types.getType(Definition);
  cv::TypeIndex List;
  uint64_t Size = 0;
  if (Type.kind() == cv::LF_UNION) {
    if (auto Record = deserialize<cv::UnionRecord>(Type)) {
      List = Record->getFieldList();
      Size = Record->getSize();
    }
  } else if (auto Record = deserialize<cv::ClassRecord>(Type)) {
    List = Record->getFieldList();
    Size = Record->getSize();
  }
  // The largest power of two that divides a value.
  const auto divisor = [](uint64_t Value) {
    return static_cast<uint32_t>(std::min<uint64_t>(Value & (~Value + 1), 16));
  };
  const auto alignUp = [](uint64_t Value, uint64_t Alignment) {
    return (Value + Alignment - 1) / Alignment * Alignment;
  };
  RecordLayout Layout;
  uint32_t Natural = 1;
  uint64_t End = 0;
  for (const FieldCollector::Data &Field : fields(List).Fields) {
    const uint32_t Alignment = alignOf(Field.Type);
    Natural = std::max(Natural, Alignment);
    End = std::max(End, Field.Offset + sizeOf(Field.Type));
    // A member, or a bitfield's storage unit, below its alignment.
    if (Alignment > 1 && Field.Offset % Alignment) {
      const uint32_t Allowed = divisor(Field.Offset);
      if (!Layout.Pack || Allowed < Layout.Pack)
        Layout.Pack = Allowed;
    }
  }
  // A size the record's alignment does not divide was packed too.
  uint32_t Aligned = Layout.Pack ? std::min(Natural, Layout.Pack) : Natural;
  if (Size && Size % Aligned) {
    const uint32_t Allowed = divisor(Size);
    if (!Layout.Pack || Allowed < Layout.Pack)
      Layout.Pack = Allowed;
    Aligned = std::min(Natural, Layout.Pack);
  }
  // A size past the members' alignment was declared aligned further.
  if (Size > alignUp(End, Aligned))
    for (uint64_t Candidate = uint64_t(Aligned) * 2;
         Candidate <= ::neverd::limits::types::kMaxInferredRecordAlignment;
         Candidate *= 2)
      if (alignUp(End, Candidate) == Size) {
        Layout.Declared = static_cast<uint32_t>(Candidate);
        break;
      }
  Layout.Alignment = Layout.Declared ? Layout.Declared : Aligned;
  return Layouts[Definition.getIndex()] = Layout;
}

TypeExprRef Importer::expr(cv::TypeIndex Index) {
  if (Index.isSimple() || Index.isNoneType())
    return exprUncached(Index);
  if (auto Found = Exprs.find(Index.getIndex()); Found != Exprs.end())
    return Found->second;
  TypeExprRef Result = exprUncached(Index);
  Exprs[Index.getIndex()] = Result;
  return Result;
}

TypeExprRef Importer::exprUncached(cv::TypeIndex Index) {
  if (Index.isNoneType())
    return makeBuiltin(Builtin::Void);
  if (Index.isSimple()) {
    auto Kind = builtinOf(Index.getSimpleKind());
    TypeExprRef Base = makeBuiltin(Kind ? *Kind : Builtin::UChar);
    if (unsigned Bytes = simplePointerBytes(Index.getSimpleMode()))
      return makePointer(std::move(Base), Bytes);
    return Base;
  }
  const cv::CVType Type = Types.getType(Index);
  switch (Type.kind()) {
  case cv::LF_POINTER: {
    auto Record = deserialize<cv::PointerRecord>(Type);
    if (!Record)
      break;
    const cv::PointerMode Mode = Record->getMode();
    TypeExprRef Pointer =
        makePointer(expr(Record->getReferentType()), Record->getSize(),
                    Mode == cv::PointerMode::LValueReference ||
                        Mode == cv::PointerMode::RValueReference);
    return qualified(std::move(Pointer), Record->isConst(),
                     Record->isVolatile());
  }
  case cv::LF_MODIFIER: {
    auto Record = deserialize<cv::ModifierRecord>(Type);
    if (!Record)
      break;
    const auto Options = static_cast<uint16_t>(Record->getModifiers());
    return qualified(
        expr(Record->getModifiedType()),
        Options & static_cast<uint16_t>(cv::ModifierOptions::Const),
        Options & static_cast<uint16_t>(cv::ModifierOptions::Volatile));
  }
  case cv::LF_ARRAY: {
    auto Record = deserialize<cv::ArrayRecord>(Type);
    if (!Record)
      break;
    const uint64_t Element = sizeOf(Record->getElementType());
    std::optional<uint64_t> Count;
    if (Element)
      Count = Record->getSize() / Element;
    return makeArray(expr(Record->getElementType()), Count);
  }
  case cv::LF_BITFIELD: {
    auto Record = deserialize<cv::BitFieldRecord>(Type);
    if (!Record)
      break;
    return expr(Record->getType());
  }
  case cv::LF_STRUCTURE:
  case cv::LF_CLASS:
  case cv::LF_INTERFACE:
  case cv::LF_UNION: {
    const bool Union = Type.kind() == cv::LF_UNION;
    llvm::StringRef Name;
    if (Union) {
      if (auto Record = deserialize<cv::UnionRecord>(Type))
        Name = Record->getName();
    } else if (auto Record = deserialize<cv::ClassRecord>(Type)) {
      Name = Record->getName();
    }
    if (isUnnamed(Name))
      if (auto Definition = definition(Index))
        return makeRecord(record(*Definition));
    return makeNamed(Name.str(), Union ? TypeKind::Union : TypeKind::Struct);
  }
  case cv::LF_ENUM: {
    auto Record = deserialize<cv::EnumRecord>(Type);
    if (!Record)
      break;
    if (isUnnamed(Record->getName()))
      if (auto Definition = definition(Index))
        return makeEnum(enumeration(*Definition));
    return makeNamed(Record->getName().str(), TypeKind::Enum);
  }
  case cv::LF_PROCEDURE: {
    auto Record = deserialize<cv::ProcedureRecord>(Type);
    if (!Record)
      break;
    if (auto Function = function(Record->getReturnType(), Record->getCallConv(),
                                 Record->getArgumentList()))
      return makeFunction(std::move(Function));
    break;
  }
  case cv::LF_MFUNCTION: {
    auto Record = deserialize<cv::MemberFunctionRecord>(Type);
    if (!Record)
      break;
    if (auto Function = function(Record->getReturnType(), Record->getCallConv(),
                                 Record->getArgumentList()))
      return makeFunction(std::move(Function));
    break;
  }
  default:
    break;
  }
  // A record kind local types do not model reads as bytes of its size.
  const uint64_t Size = sizeOf(Index);
  return Size ? makeArray(makeBuiltin(Builtin::UChar), Size)
              : makeBuiltin(Builtin::Void);
}

FieldCollector Importer::fields(cv::TypeIndex List) {
  FieldCollector Collector;
  std::optional<cv::TypeIndex> Next = List;
  // A long field list continues in another, which a well-formed stream never
  // loops back from; the bound guards a malformed one.
  for (unsigned Lists = 0;
       Next && !Next->isNoneType() &&
       Lists < ::neverd::limits::types::kPDBTypeImportImporterFieldsListsLimit;
       ++Lists) {
    const cv::CVType Type = Types.getType(*Next);
    Collector.Continuation.reset();
    if (Type.kind() != cv::LF_FIELDLIST)
      break;
    auto Record = deserialize<cv::FieldListRecord>(Type);
    if (!Record)
      break;
    if (auto Err = cv::visitMemberRecordStream(Record->Data, Collector)) {
      llvm::consumeError(std::move(Err));
      break;
    }
    Next = Collector.Continuation;
  }
  return Collector;
}

std::shared_ptr<const RecordDef> Importer::record(cv::TypeIndex Definition) {
  const cv::CVType Type = Types.getType(Definition);
  auto Result = std::make_shared<RecordDef>();
  cv::TypeIndex List;
  if (Type.kind() == cv::LF_UNION) {
    auto Record = deserialize<cv::UnionRecord>(Type);
    if (!Record)
      return Result;
    Result->Union = true;
    Result->Size = Record->getSize();
    List = Record->getFieldList();
  } else {
    auto Record = deserialize<cv::ClassRecord>(Type);
    if (!Record)
      return Result;
    Result->Size = Record->getSize();
    List = Record->getFieldList();
  }
  const RecordLayout &Layout = layoutOf(Definition);
  Result->Pack = Layout.Pack;
  Result->Alignment = Layout.Declared;
  FieldCollector Collector = fields(List);
  std::vector<FlatMember> Flat;
  Flat.reserve(Collector.Fields.size());
  for (const FieldCollector::Data &Field : Collector.Fields) {
    FlatMember Member;
    Member.Name = Field.Name;
    Member.BitOffset = Field.Offset * 8;
    Member.StorageBitOffset = Member.BitOffset;
    if (!Field.Type.isSimple() &&
        Types.getType(Field.Type).kind() == cv::LF_BITFIELD) {
      if (auto Bits =
              deserialize<cv::BitFieldRecord>(Types.getType(Field.Type))) {
        Member.Type = expr(Bits->getType());
        Member.BitWidth = Bits->getBitSize();
        Member.BitOffset += Bits->getBitOffset();
        Member.StorageBits = sizeOf(Bits->getType()) * 8;
        Member.Alignment = alignOf(Bits->getType());
      }
    }
    if (!Member.BitWidth) {
      Member.Type = expr(Field.Type);
      Member.StorageBits = sizeOf(Field.Type) * 8;
      Member.Alignment = alignOf(Field.Type);
    }
    if (Result->Pack)
      Member.Alignment = std::min(Member.Alignment, Result->Pack);
    Flat.push_back(std::move(Member));
  }
  Result->Members = nestFlatMembers(Flat, Result->Union, Result->Pack);
  // The nesting must lay the members out exactly as MSVC listed them.
  const std::vector<FlatMember> Back = flattenMembers(*Result);
  bool Holds = Back.size() == Flat.size();
  for (size_t I = 0; Holds && I < Back.size(); ++I)
    Holds = Back[I].Name == Flat[I].Name &&
            Back[I].BitOffset == Flat[I].BitOffset &&
            Back[I].BitWidth == Flat[I].BitWidth;
  if (!Holds)
    ++Mismatches;
  return Result;
}

std::shared_ptr<const EnumDef> Importer::enumeration(cv::TypeIndex Definition) {
  auto Result = std::make_shared<EnumDef>();
  auto Record = deserialize<cv::EnumRecord>(Types.getType(Definition));
  if (!Record)
    return Result;
  const cv::TypeIndex Underlying = Record->getUnderlyingType();
  if (Underlying.isSimple())
    if (auto Kind = builtinOf(Underlying.getSimpleKind()))
      Result->Underlying = *Kind;
  Result->Values = fields(Record->getFieldList()).Enumerators;
  // CodeView may spell -1 as 0xFFFFFFFF; a value takes the underlying
  // type's width and, when that type is signed, its sign.
  const unsigned Bits = Library.builtinBytes(Result->Underlying) * 8;
  if (Bits && Bits < 64)
    for (Enumerator &Value : Result->Values) {
      const uint64_t Mask = (uint64_t(1) << Bits) - 1;
      uint64_t Truncated = Value.Value & Mask;
      if (builtinInfo(Result->Underlying).Signed &&
          (Truncated >> (Bits - 1)) & 1)
        Truncated |= ~Mask;
      Value.Value = Truncated;
    }
  return Result;
}

std::shared_ptr<const FunctionDef>
Importer::function(cv::TypeIndex Return, cv::CallingConvention Convention,
                   cv::TypeIndex Arguments) {
  const auto SourceConvention =
      codeViewCallingConvention(Convention, pointerBytes(Library.dataModel()));
  if (!SourceConvention) {
    ++UnsupportedFunctionTypes;
    return nullptr;
  }
  auto Result = std::make_shared<FunctionDef>();
  Result->Convention = *SourceConvention;
  Result->Return = expr(Return);
  if (Arguments.isSimple() || Arguments.isNoneType())
    return Result;
  auto List = deserialize<cv::ArgListRecord>(Types.getType(Arguments));
  if (!List)
    return Result;
  for (cv::TypeIndex Argument : List->getIndices()) {
    // CodeView ends a variadic list with an argument of no type.
    if (Argument.isNoneType()) {
      Result->Variadic = true;
      continue;
    }
    Result->Params.push_back({std::string(), expr(Argument)});
  }
  return Result;
}

PDBTypeImportStats Importer::run() {
  PDBTypeImportStats Stats;
  const uint32_t First = Tpi.TypeIndexBegin();
  const uint32_t Count = Tpi.getNumTypeRecords();
  // Where each union, structure and enumeration is defined.  A name the
  // stream defines more than once, as the kernel defines the architectural
  // part of _KPRCB apart from the whole, takes its largest definition.
  llvm::DenseMap<uint32_t, uint64_t> DefinedSizes;
  const auto choose = [&](llvm::StringMap<cv::TypeIndex> &Map,
                          llvm::StringRef Key, cv::TypeIndex Index,
                          uint64_t Size) {
    auto [It, Inserted] = Map.try_emplace(Key, Index);
    if (!Inserted && Size > DefinedSizes.lookup(It->second.getIndex()))
      It->second = Index;
  };
  for (uint32_t I = 0; I < Count; ++I) {
    const cv::TypeIndex Index(First + I);
    const cv::CVType Type = Types.getType(Index);
    llvm::StringRef Name, Unique;
    bool Forward = true;
    uint64_t Size = 0;
    if (isClassKind(Type.kind())) {
      if (auto Record = deserialize<cv::ClassRecord>(Type)) {
        Forward = Record->isForwardRef();
        Name = Record->getName();
        Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
        Size = Record->getSize();
      }
    } else if (Type.kind() == cv::LF_UNION) {
      if (auto Record = deserialize<cv::UnionRecord>(Type)) {
        Forward = Record->isForwardRef();
        Name = Record->getName();
        Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
        Size = Record->getSize();
      }
    } else if (Type.kind() == cv::LF_ENUM) {
      if (auto Record = deserialize<cv::EnumRecord>(Type)) {
        Forward = Record->isForwardRef();
        Name = Record->getName();
        Unique = Record->hasUniqueName() ? Record->getUniqueName() : "";
      }
    }
    if (Forward)
      continue;
    DefinedSizes[Index.getIndex()] = Size;
    if (!Unique.empty())
      choose(ByUniqueName, Unique, Index, Size);
    choose(ByName, Name, Index, Size);
  }

  // Every named type, in the order of its first record, defined once.
  for (uint32_t I = 0; I < Count; ++I) {
    const cv::TypeIndex Index(First + I);
    const cv::CVType Type = Types.getType(Index);
    const bool Class = isClassKind(Type.kind());
    if (!Class && Type.kind() != cv::LF_UNION && Type.kind() != cv::LF_ENUM)
      continue;
    llvm::StringRef Name;
    if (Class) {
      if (auto Record = deserialize<cv::ClassRecord>(Type))
        Name = Record->getName();
    } else if (Type.kind() == cv::LF_UNION) {
      if (auto Record = deserialize<cv::UnionRecord>(Type))
        Name = Record->getName();
    } else if (auto Record = deserialize<cv::EnumRecord>(Type)) {
      Name = Record->getName();
    }
    if (isUnnamed(Name) || Library.find(Name))
      continue;
    LocalType Local;
    Local.Name = Name.str();
    Local.Origin = TypeOrigin::PDB;
    // The definition the name takes, wherever its first record is.
    std::optional<cv::TypeIndex> Definition = definition(Index);
    if (auto Chosen = ByName.find(Name); Chosen != ByName.end())
      Definition = Chosen->second;
    if (Type.kind() == cv::LF_ENUM) {
      Local.Kind = TypeKind::Enum;
      if (!Definition) {
        ++Stats.DeclaredOnly;
        Local.Enum = std::make_shared<EnumDef>();
      } else {
        Local.Enum = enumeration(*Definition);
        ++Stats.Enums;
      }
      Library.define(std::move(Local));
      continue;
    }
    Local.Kind = Class ? TypeKind::Struct : TypeKind::Union;
    if (!Definition) {
      auto Declared = std::make_shared<RecordDef>();
      Declared->Union = !Class;
      Declared->Defined = false;
      Local.Record = std::move(Declared);
      ++Stats.DeclaredOnly;
    } else {
      const unsigned Before = Mismatches;
      Local.Record = record(*Definition);
      if (Mismatches != Before)
        Stats.NestingMismatches.push_back(Local.Name);
      ++(Class ? Stats.Structs : Stats.Unions);
    }
    Library.define(std::move(Local));
  }
  Stats.UnsupportedFunctionTypes = UnsupportedFunctionTypes;
  return Stats;
}

} // namespace

llvm::Expected<PDBTypeImportStats> importPDBTypes(llvm::pdb::PDBFile &File,
                                                  TypeLibrary &Into) {
  if (!File.hasPDBTpiStream())
    return PDBTypeImportStats();
  auto Tpi = File.getPDBTpiStream();
  if (!Tpi)
    return Tpi.takeError();
  PDBTypeImportStats Stats = Importer(*Tpi, Into).run();
  // Layout needs every type's size, so it waits for the whole stream.
  for (const LocalType *Type : Into.types())
    if (Type->Record && Type->Record->Defined &&
        !layoutProblem(Into, *Type->Record).empty() &&
        !llvm::is_contained(Stats.NestingMismatches, Type->Name))
      Stats.NestingMismatches.push_back(Type->Name);
  return Stats;
}

llvm::Expected<std::unique_ptr<TypeLibrary>>
importPDBTypes(llvm::pdb::PDBFile &File, PDBTypeImportStats *Stats) {
  DataModel Model = DataModel::LLP64;
  if (File.hasPDBDbiStream()) {
    auto Dbi = File.getPDBDbiStream();
    if (!Dbi)
      return Dbi.takeError();
    Model = dataModelOf(Dbi->getMachineType());
  }
  auto Library = std::make_unique<TypeLibrary>(Model);
  auto Read = importPDBTypes(File, *Library);
  if (!Read)
    return Read.takeError();
  if (Stats)
    *Stats = *Read;
  return std::move(Library);
}

llvm::Expected<std::unique_ptr<TypeLibrary>>
importPDBTypes(llvm::StringRef Path, PDBTypeImportStats *Stats) {
  std::unique_ptr<llvm::pdb::IPDBSession> Session;
  if (auto Err = llvm::pdb::loadDataForPDB(llvm::pdb::PDB_ReaderType::Native,
                                           Path, Session))
    return std::move(Err);
  return importPDBTypes(
      static_cast<llvm::pdb::NativeSession &>(*Session).getPDBFile(), Stats);
}

} // namespace neverd::types
