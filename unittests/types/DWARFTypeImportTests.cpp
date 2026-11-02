//===- DWARFTypeImportTests.cpp - Local types from DWARF type entries -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "TypeShape.h"

#include "neverd/types/DWARFTypeImport.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <gtest/gtest.h>
#include <initializer_list>
#include <limits>

using namespace neverd::types;
using neverd::types::test::shapeOf;

namespace {

// A signed subrange in raw DWARF exercises boundary values without asking a
// compiler to create an object of that size.
llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>>
arrayBoundSections(int64_t Lower, int64_t Upper) {
  using namespace llvm::dwarf;
  llvm::SmallString<128> Abbrev, Info;
  llvm::raw_svector_ostream A(Abbrev), I(Info);
  const auto abbrev = [&](unsigned Code, unsigned Tag, bool Children,
                          std::initializer_list<unsigned> Attributes) {
    llvm::encodeULEB128(Code, A);
    llvm::encodeULEB128(Tag, A);
    A << char(Children);
    for (unsigned Attribute : Attributes)
      llvm::encodeULEB128(Attribute, A);
    A << char(0) << char(0);
  };
  abbrev(1, DW_TAG_compile_unit, true, {});
  abbrev(2, DW_TAG_base_type, false,
         {DW_AT_name, DW_FORM_string, DW_AT_encoding, DW_FORM_data1,
          DW_AT_byte_size, DW_FORM_data1});
  abbrev(3, DW_TAG_array_type, true, {DW_AT_type, DW_FORM_ref4});
  abbrev(4, DW_TAG_subrange_type, false,
         {DW_AT_lower_bound, DW_FORM_sdata, DW_AT_upper_bound, DW_FORM_sdata});
  abbrev(5, DW_TAG_typedef, false,
         {DW_AT_name, DW_FORM_string, DW_AT_type, DW_FORM_ref4});
  A << char(0);
  const auto u32 = [&](uint32_t Value) {
    char Bytes[4];
    llvm::support::endian::write32le(Bytes, Value);
    I.write(Bytes, sizeof(Bytes));
  };
  u32(0);
  I << char(4) << char(0);
  u32(0);
  I << char(8) << char(1);
  const uint32_t ElementOffset = Info.size();
  I << char(2) << "unsigned char" << char(0) << char(DW_ATE_unsigned_char)
    << char(1);
  const uint32_t ArrayOffset = Info.size();
  I << char(3);
  u32(ElementOffset);
  I << char(4);
  llvm::encodeSLEB128(Lower, I);
  llvm::encodeSLEB128(Upper, I);
  I << char(0) << char(5) << "BoundedArray" << char(0);
  u32(ArrayOffset);
  I << char(0);
  llvm::support::endian::write32le(Info.data(), Info.size() - 4);
  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> Sections;
  Sections["debug_abbrev"] = llvm::MemoryBuffer::getMemBufferCopy(Abbrev);
  Sections["debug_info"] = llvm::MemoryBuffer::getMemBufferCopy(Info);
  return Sections;
}

/// A Go func value with unmarked parameters, as the legacy writer emits,
/// or with explicit input markers.
/// Each parameter names bool or *bool; the typedef makes the value visible
/// to the importer without requiring an installed Go toolchain.
llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>>
goFunctionSections(llvm::StringRef Name, llvm::ArrayRef<bool> PointerParams,
                   bool InputMarkers = false) {
  using namespace llvm::dwarf;
  llvm::SmallString<128> Abbrev, Info;
  llvm::raw_svector_ostream A(Abbrev), I(Info);
  const auto abbrev = [&](unsigned Code, unsigned Tag, bool Children,
                          std::initializer_list<unsigned> Attributes) {
    llvm::encodeULEB128(Code, A);
    llvm::encodeULEB128(Tag, A);
    A << char(Children);
    for (unsigned Attribute : Attributes)
      llvm::encodeULEB128(Attribute, A);
    A << char(0) << char(0);
  };
  abbrev(1, DW_TAG_compile_unit, true, {DW_AT_language, DW_FORM_data2});
  abbrev(2, DW_TAG_base_type, false,
         {DW_AT_name, DW_FORM_string, DW_AT_encoding, DW_FORM_data1,
          DW_AT_byte_size, DW_FORM_data1});
  abbrev(3, DW_TAG_pointer_type, false, {DW_AT_type, DW_FORM_ref4});
  abbrev(4, DW_TAG_subroutine_type, true,
         {DW_AT_name, DW_FORM_string, DW_AT_byte_size, DW_FORM_data1});
  if (InputMarkers)
    abbrev(5, DW_TAG_formal_parameter, false,
           {DW_AT_type, DW_FORM_ref4, DW_AT_variable_parameter, DW_FORM_flag});
  else
    abbrev(5, DW_TAG_formal_parameter, false, {DW_AT_type, DW_FORM_ref4});
  abbrev(6, DW_TAG_typedef, false,
         {DW_AT_name, DW_FORM_string, DW_AT_type, DW_FORM_ref4});
  A << char(0);
  const auto u32 = [&](uint32_t Value) {
    char Bytes[4];
    llvm::support::endian::write32le(Bytes, Value);
    I.write(Bytes, sizeof(Bytes));
  };
  u32(0);                  // Unit length, patched below.
  I << char(4) << char(0); // DWARF version.
  u32(0);                  // Abbreviation offset.
  I << char(8) << char(1) << char(DW_LANG_Go) << char(0);
  const uint32_t BoolOffset = Info.size();
  I << char(2) << "bool" << char(0) << char(DW_ATE_boolean) << char(1);
  const uint32_t PointerOffset = Info.size();
  I << char(3);
  u32(BoolOffset);
  const uint32_t FunctionOffset = Info.size();
  I << char(4) << Name << char(0) << char(8);
  for (bool Pointer : PointerParams) {
    I << char(5);
    u32(Pointer ? PointerOffset : BoolOffset);
    if (InputMarkers)
      I << char(0);
  }
  I << char(0) << char(6) << "Callback" << char(0);
  u32(FunctionOffset);
  I << char(0);
  llvm::support::endian::write32le(Info.data(), Info.size() - 4);
  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> Sections;
  Sections["debug_abbrev"] = llvm::MemoryBuffer::getMemBufferCopy(Abbrev);
  Sections["debug_info"] = llvm::MemoryBuffer::getMemBufferCopy(Info);
  return Sections;
}

/// The types of the object \p Name, of the fixtures, which a compiler wrote
/// with DWARF.
class DWARFTypeImport : public ::testing::TestWithParam<const char *> {
protected:
  void SetUp() override {
    auto Object = llvm::object::ObjectFile::createObjectFile(
        std::string(NEVERD_TYPES_FIXTURES "/") + GetParam());
    ASSERT_TRUE(static_cast<bool>(Object))
        << llvm::toString(Object.takeError());
    Owner = std::move(*Object);
    Context = llvm::DWARFContext::create(*Owner.getBinary());
    auto Read = importDWARFTypes(*Context, DataModel::LP64, &Stats);
    ASSERT_TRUE(static_cast<bool>(Read)) << llvm::toString(Read.takeError());
    Library = std::move(*Read);
  }

  const RecordDef &recordOf(llvm::StringRef Name) const {
    const LocalType *Type = Library->find(Name);
    EXPECT_NE(Type, nullptr) << Name.str();
    static const RecordDef Empty;
    return Type && Type->Record ? *Type->Record : Empty;
  }

  llvm::object::OwningBinary<llvm::object::ObjectFile> Owner;
  std::unique_ptr<llvm::DWARFContext> Context;
  DWARFTypeImportStats Stats;
  std::unique_ptr<TypeLibrary> Library;
};

} // namespace

TEST_P(DWARFTypeImport, ReadsEveryNamedType) {
  EXPECT_TRUE(Stats.Skipped.empty()) << Stats.Skipped.front();
  EXPECT_TRUE(Stats.Conflicts.empty());
  for (const char *Name :
       {"list", "pair_t", "value", "value_t", "color", "mode_t_", "flags",
        "device", "packed", "aligned", "tail", "uses_forward"})
    EXPECT_NE(Library->find(Name), nullptr) << Name;
  for (const LocalType *Type : Library->types())
    EXPECT_EQ(Type->Origin, TypeOrigin::DWARF) << Type->Name;
  EXPECT_EQ(Library->find("value")->Kind, TypeKind::Union);
  // A typedef of an anonymous record names it, and one of a record of
  // another name stays a typedef.
  EXPECT_EQ(Library->find("pair_t")->Kind, TypeKind::Struct);
  EXPECT_EQ(Library->find("mode_t_")->Kind, TypeKind::Enum);
  const LocalType *Value = Library->find("value_t");
  ASSERT_EQ(Value->Kind, TypeKind::Typedef);
  EXPECT_EQ(Value->Target->Name, "value");
  EXPECT_EQ(Value->Target->NamedKind, TypeKind::Union);
  // A record no unit defines is declared.
  const LocalType *Forward = Library->find("forward_only");
  ASSERT_NE(Forward, nullptr);
  EXPECT_FALSE(Forward->Record->Defined);
  EXPECT_EQ(Stats.DeclaredOnly, 1u);
}

TEST_P(DWARFTypeImport, KeepsAnonymousRecordsInPlace) {
  EXPECT_EQ(shapeOf(recordOf("value")),
            "union{struct@0{lo@0 hi@4} pair@0 quad@0}");
  EXPECT_EQ(recordOf("value").Size, 8u);
}

TEST_P(DWARFTypeImport, KeepsBitFieldsWhereTheCompilerPutThem) {
  // GNU C fills the int that holds the byte before a bit-field.
  const RecordDef &Flags = recordOf("flags");
  EXPECT_EQ(shapeOf(Flags), "struct{kind@0 ready@8b:1 level@9b:3 delta@12b:4 "
                            "wide@16b:40 tail@8}");
  EXPECT_EQ(Flags.Size, 16u);
  const Member &Delta = Flags.Members[3];
  EXPECT_EQ(Delta.StorageBitOffset, 0u);
  EXPECT_EQ(Delta.Type->Name, "int32_t");
  EXPECT_TRUE(layoutProblem(*Library, Flags).empty());
}

TEST_P(DWARFTypeImport, KeepsOffsetsSizesAndMemberTypes) {
  const RecordDef &Device = recordOf("device");
  EXPECT_EQ(shapeOf(Device),
            "struct{type@0 size@2 links@8 dispatch@24 time@56 color@64 "
            "mode@68 flags@72 name@88 status@96 precise@112 on@128}");
  EXPECT_EQ(Device.Size, 144u);
  const Member &Dispatch = Device.Members[3];
  ASSERT_EQ(Dispatch.Type->Kind, TypeExprKind::Array);
  EXPECT_EQ(Dispatch.Type->Count, std::optional<uint64_t>(4));
  const TypeExpr &Pointer = *Dispatch.Type->Element;
  ASSERT_EQ(Pointer.Kind, TypeExprKind::Pointer);
  ASSERT_EQ(Pointer.Element->Kind, TypeExprKind::Function);
  const FunctionDef &Function = *Pointer.Element->Function;
  ASSERT_EQ(Function.Params.size(), 2u);
  EXPECT_EQ(Function.Params[0].Type->Element->Name, "device");
  EXPECT_EQ(Function.Return->TheBuiltin, Builtin::Int);
  // An array of arrays, the outer first.
  const TypeExpr &Name = *Device.Members[8].Type;
  ASSERT_EQ(Name.Kind, TypeExprKind::Array);
  EXPECT_EQ(Name.Count, std::optional<uint64_t>(2));
  EXPECT_EQ(Name.Element->Count, std::optional<uint64_t>(3));
  EXPECT_TRUE(Device.Members[9].Type->Const);
  EXPECT_TRUE(Device.Members[9].Type->Volatile);
  EXPECT_EQ(Device.Members[10].Type->TheBuiltin, Builtin::LongDouble);
  EXPECT_EQ(Device.Members[11].Type->TheBuiltin, Builtin::Bool);
  EXPECT_TRUE(layoutProblem(*Library, Device).empty());
}

TEST_P(DWARFTypeImport, ReadsEnumerations) {
  const LocalType *Color = Library->find("color");
  ASSERT_NE(Color, nullptr);
  ASSERT_EQ(Color->Enum->Values.size(), 3u);
  EXPECT_EQ(Color->Enum->Values[1].Name, "GREEN");
  EXPECT_EQ(Color->Enum->Values[1].Value, 5u);
  EXPECT_EQ(static_cast<int64_t>(Color->Enum->Values[2].Value), -1);
  EXPECT_TRUE(builtinInfo(Color->Enum->Underlying).Signed);
  EXPECT_EQ(builtinBytes(Color->Enum->Underlying, DataModel::LP64,
                         RecordABI::Itanium),
            4u);
}

TEST_P(DWARFTypeImport, InfersPackingAlignmentAndFlexibleArrays) {
  const RecordDef &Packed = recordOf("packed");
  EXPECT_EQ(Packed.Size, 7u);
  EXPECT_EQ(Packed.Pack, 1u);
  EXPECT_EQ(shapeOf(Packed), "struct{tag@0 value@1 count@5}");
  const RecordDef &Aligned = recordOf("aligned");
  EXPECT_EQ(Aligned.Size, 16u);
  EXPECT_EQ(Aligned.Alignment, 16u);
  const RecordDef &Tail = recordOf("tail");
  EXPECT_EQ(Tail.Size, 4u);
  EXPECT_EQ(Tail.Members[1].Type->Count, std::nullopt);
}

TEST_P(DWARFTypeImport, RendersInEveryLanguage) {
  // The C, and each language's rendering, checks every layout.
  for (Language Lang : {Language::C, Language::Rust, Language::Go}) {
    RenderOptions Options;
    Options.Lang = Lang;
    Options.Asserts = true;
    const std::string Text = renderTypes(*Library, Library->types(), Options);
    EXPECT_FALSE(Text.empty());
  }
}

INSTANTIATE_TEST_SUITE_P(Compilers, DWARFTypeImport,
                         ::testing::Values("dwarf_types.gcc.o",
                                           "dwarf_types.clang.o"));

TEST(DWARFTypeImportRust, ReadsRustsLayoutsAndHoldsVariantsAsBytes) {
  auto Object = llvm::object::ObjectFile::createObjectFile(NEVERD_TYPES_FIXTURES
                                                           "/rust_dwarf.o");
  ASSERT_TRUE(static_cast<bool>(Object)) << llvm::toString(Object.takeError());
  const std::unique_ptr<llvm::DWARFContext> Context =
      llvm::DWARFContext::create(*Object->getBinary());
  DWARFTypeImportStats Stats;
  auto Read = importDWARFTypes(*Context, DataModel::LP64, &Stats);
  ASSERT_TRUE(static_cast<bool>(Read)) << llvm::toString(Read.takeError());
  const TypeLibrary &Library = **Read;
  // Types are named within their crate's namespace.
  const LocalType *Point = Library.find("rust_dwarf::Point");
  ASSERT_NE(Point, nullptr);
  EXPECT_EQ(shapeOf(*Point->Record), "struct{x@0 y@8}");
  // A structure Rust reorders holds its fields by offset.
  const LocalType *Plain = Library.find("rust_dwarf::Plain");
  ASSERT_NE(Plain, nullptr);
  EXPECT_EQ(shapeOf(*Plain->Record), "struct{b@0 c@4 a@6}");
  EXPECT_EQ(Plain->Record->Size, 8u);
  const LocalType *Kind = Library.find("rust_dwarf::Kind");
  ASSERT_NE(Kind, nullptr);
  EXPECT_EQ(Kind->Enum->Underlying, Builtin::UChar);
  ASSERT_EQ(Kind->Enum->Values.size(), 2u);
  EXPECT_EQ(Kind->Enum->Values[1].Value, 2u);
  // An enumeration with data is variants, which local types do not spell:
  // its bytes, as large and aligned as the enumeration, so that what holds
  // it lays out; its variants' own structures stay.
  const LocalType *Shape = Library.find("rust_dwarf::Shape");
  ASSERT_NE(Shape, nullptr);
  ASSERT_TRUE(Shape->Record);
  EXPECT_EQ(shapeOf(*Shape->Record), "struct{__variants@0}");
  EXPECT_EQ(Shape->Record->Size, 16u);
  EXPECT_EQ(Shape->Record->Alignment, 8u);
  EXPECT_NE(Shape->Comment.find("Rust enumeration"), std::string::npos);
  EXPECT_TRUE(Stats.Skipped.empty());
  EXPECT_NE(Library.find("rust_dwarf::Shape::Square"), nullptr);
  // Members of no bytes, () and PhantomData, lie nowhere.
  const LocalType *Holder = Library.find("rust_dwarf::Holder");
  ASSERT_NE(Holder, nullptr);
  EXPECT_EQ(shapeOf(*Holder->Record), "struct{shape@0 tag@16}");
  EXPECT_EQ(Holder->Record->Size, 24u);
  // A function item holds no bytes, though DWARF gives it as a pointer to
  // its function, past the record's end.
  const LocalType *Mapper = Library.find("rust_dwarf::Mapper<fn(u32) -> u32>");
  ASSERT_NE(Mapper, nullptr);
  EXPECT_EQ(shapeOf(*Mapper->Record), "struct{base@0}");
  EXPECT_EQ(Mapper->Record->Size, 4u);
  // An unsized tail, which DWARF gives as its element, is C's flexible
  // array.
  const LocalType *Tail = Library.find("rust_dwarf::Tail");
  ASSERT_NE(Tail, nullptr);
  EXPECT_EQ(shapeOf(*Tail->Record), "struct{len@0 data@4}");
  EXPECT_EQ(Tail->Record->Size, 4u);
  ASSERT_EQ(Tail->Record->Members.size(), 2u);
  const TypeExpr &Data = *Tail->Record->Members[1].Type;
  EXPECT_EQ(Data.Kind, TypeExprKind::Array);
  EXPECT_FALSE(Data.Count.has_value());
}

TEST(DWARFTypeImportGo, ReadsGosStructuresAndFuncValues) {
  std::string Go;
  if (const char *FromEnv = std::getenv("NEVERD_TEST_GO"))
    Go = FromEnv;
  else if (auto Found = llvm::sys::findProgramByName("go"))
    Go = *Found;
  if (Go.empty())
    GTEST_SKIP() << "no go: no Go program to read the DWARF of";
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-go-dwarf", Dir));
  const auto path = [&](llvm::StringRef Name) {
    llvm::SmallString<128> Path(Dir);
    llvm::sys::path::append(Path, Name);
    return std::string(Path);
  };
  {
    std::error_code Error;
    llvm::raw_fd_ostream Source(path("main.go"), Error);
    ASSERT_FALSE(Error);
    Source << "package main\n"
              "type Point struct {\n\tX int32\n\tY int64\n\tName string\n}\n"
              "type Guarded struct {\n\tlock noCopy\n\tV int32\n}\n"
              "type noCopy struct{}\n"
              "type Off struct {\n\ta uintptr\n}\n"
              "type Span struct {\n\tbase, limit Off\n}\n"
              "type Spans struct {\n\tx uint64\n\ts Span\n}\n"
              "type Calls struct {\n\t_ [2]byte\n"
              "\tEqual func(a, b uintptr) bool\n\t_ [6]byte\n"
              "\tPair func() (int, bool)\n\tCount int32\n"
              "\tPointer func(*bool) *bool\n"
              "\tProcedure func(*bool)\n"
              "\tNested func(func(int, int) bool, *bool) *bool\n"
              "\tTagged func(struct { Value int `json:\",)\"` }) bool\n"
              "\tVariadic func(...*bool) bool\n}\n"
              "//go:noinline\n"
              "func area(p *Point) int64 { return int64(p.X) * p.Y }\n"
              "//go:noinline\n"
              "func calls(c *Calls) int32 { return c.Count }\n"
              "//go:noinline\n"
              "func guarded(g *Guarded) int32 { return g.V }\n"
              "//go:noinline\n"
              "func spans(s *Spans) uint64 { return s.x }\n"
              "func main() {\n\tprintln(area(&Point{X: 2, Y: 3}))\n"
              "\tprintln(calls(&Calls{Count: 4}))\n"
              "\tprintln(guarded(&Guarded{V: 5}))\n"
              "\tprintln(spans(&Spans{x: 6}))\n}\n";
  }
  const std::string Program = path("program");
  const std::string Output = path("go.txt");
  const std::string Main = path("main.go");
  // The linker compresses DWARF, which the session expands
  // (CompressedSections.h) before it reads it; read here as written.
  const llvm::StringRef Argv[] = {
      Go, "build", "-ldflags=-compressdwarf=false", "-o", Program, Main};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Output};
  std::string Message;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(Go, Argv, std::nullopt, Redirects, 300, 0,
                                      &Message),
            0)
      << Message;
  auto Object = llvm::object::ObjectFile::createObjectFile(Program);
  ASSERT_TRUE(static_cast<bool>(Object)) << llvm::toString(Object.takeError());
  const std::unique_ptr<llvm::DWARFContext> Context =
      llvm::DWARFContext::create(*Object->getBinary());
  DWARFTypeImportStats Stats;
  auto Read = importDWARFTypes(*Context, DataModel::LP64, &Stats);
  ASSERT_TRUE(static_cast<bool>(Read)) << llvm::toString(Read.takeError());
  const TypeLibrary &Library = **Read;
  const LocalType *Point = Library.find("main.Point");
  ASSERT_NE(Point, nullptr);
  EXPECT_EQ(shapeOf(*Point->Record), "struct{X@0 Y@8 Name@16}");
  EXPECT_EQ(Point->Record->Size, 32u);
  // A Go string is a structure of its own.
  const LocalType *String = Library.find("string");
  ASSERT_NE(String, nullptr);
  EXPECT_EQ(String->Record->Size, 16u);
  // Blank fields take numbers past their first; a func value is the
  // address of its closure, a pointer to its function, whose one result
  // Go lists as a parameter, and one of several results an address alone.
  // A member of no bytes lies nowhere, whichever type DWARF gives first.
  const LocalType *Guarded = Library.find("main.Guarded");
  ASSERT_NE(Guarded, nullptr);
  EXPECT_EQ(shapeOf(*Guarded->Record), "struct{V@0}");
  EXPECT_EQ(Guarded->Record->Size, 4u);
  // A record held by value through the typedef of its own name, as Go
  // names each type, lays out before what holds it: no alignment is
  // guessed of members not read yet.
  const LocalType *Span = Library.find("main.Span");
  ASSERT_NE(Span, nullptr);
  EXPECT_EQ(Span->Record->Alignment, 0u);
  EXPECT_EQ(Library.alignOf(*makeNamed("main.Span", TypeKind::Struct)), 8u);
  const LocalType *Spans = Library.find("main.Spans");
  ASSERT_NE(Spans, nullptr);
  EXPECT_EQ(shapeOf(*Spans->Record), "struct{x@0 s@8}");
  const LocalType *Calls = Library.find("main.Calls");
  ASSERT_NE(Calls, nullptr);
  EXPECT_EQ(shapeOf(*Calls->Record), "struct{_@0 Equal@8 _1@16 Pair@24 "
                                     "Count@32 Pointer@40 Procedure@48 "
                                     "Nested@56 Tagged@64 Variadic@72}");
  const auto pointee = [&](llvm::StringRef Wanted) -> const TypeExpr * {
    for (const Member &Field : Calls->Record->Members)
      if (Field.Name == Wanted) {
        const TypeExpr *Type = Field.Type.get();
        while (Type && Type->Kind == TypeExprKind::Named)
          if (const LocalType *Named = Library.find(Type->Name);
              Named && Named->Kind == TypeKind::Typedef)
            Type = Named->Target.get();
          else
            break;
        return Type && Type->Kind == TypeExprKind::Pointer ? Type->Element.get()
                                                           : nullptr;
      }
    return nullptr;
  };
  const TypeExpr *Equal = pointee("Equal");
  ASSERT_NE(Equal, nullptr);
  ASSERT_EQ(Equal->Kind, TypeExprKind::Function);
  ASSERT_TRUE(Equal->Function);
  EXPECT_EQ(Equal->Function->Params.size(), 2u);
  ASSERT_TRUE(Equal->Function->Return);
  EXPECT_EQ(Equal->Function->Return->Kind, TypeExprKind::Builtin);
  EXPECT_EQ(Equal->Function->Return->TheBuiltin, Builtin::Bool);
  const TypeExpr *Pair = pointee("Pair");
  ASSERT_NE(Pair, nullptr);
  EXPECT_EQ(Pair->Kind, TypeExprKind::Builtin);
  EXPECT_EQ(Pair->TheBuiltin, Builtin::Void);
  // Legacy output pointers are unwrapped once; input pointers and a
  // pointer-valued result keep their own indirection.
  const TypeExpr *Pointer = pointee("Pointer");
  ASSERT_NE(Pointer, nullptr);
  ASSERT_EQ(Pointer->Kind, TypeExprKind::Function);
  ASSERT_EQ(Pointer->Function->Params.size(), 1u);
  EXPECT_EQ(Pointer->Function->Params[0].Type->Kind, TypeExprKind::Pointer);
  ASSERT_EQ(Pointer->Function->Return->Kind, TypeExprKind::Pointer);
  EXPECT_EQ(Pointer->Function->Return->Element->TheBuiltin, Builtin::Bool);
  const TypeExpr *Procedure = pointee("Procedure");
  ASSERT_NE(Procedure, nullptr);
  ASSERT_EQ(Procedure->Kind, TypeExprKind::Function);
  ASSERT_EQ(Procedure->Function->Params.size(), 1u);
  EXPECT_EQ(Procedure->Function->Params[0].Type->Kind, TypeExprKind::Pointer);
  EXPECT_EQ(Procedure->Function->Return->TheBuiltin, Builtin::Void);
  const TypeExpr *Nested = pointee("Nested");
  ASSERT_NE(Nested, nullptr);
  ASSERT_EQ(Nested->Kind, TypeExprKind::Function);
  ASSERT_EQ(Nested->Function->Params.size(), 2u);
  EXPECT_EQ(Nested->Function->Params[0].Type->Kind, TypeExprKind::Named);
  EXPECT_EQ(Nested->Function->Params[1].Type->Kind, TypeExprKind::Pointer);
  EXPECT_EQ(Nested->Function->Return->Kind, TypeExprKind::Pointer);
  const TypeExpr *Tagged = pointee("Tagged");
  ASSERT_NE(Tagged, nullptr);
  ASSERT_EQ(Tagged->Kind, TypeExprKind::Function);
  EXPECT_EQ(Tagged->Function->Params.size(), 1u);
  EXPECT_EQ(Tagged->Function->Return->TheBuiltin, Builtin::Bool);
  const TypeExpr *Variadic = pointee("Variadic");
  ASSERT_NE(Variadic, nullptr);
  ASSERT_EQ(Variadic->Kind, TypeExprKind::Function);
  EXPECT_TRUE(Variadic->Function->Variadic);
  EXPECT_EQ(Variadic->Function->Params.size(), 1u);
  EXPECT_EQ(Variadic->Function->Return->TheBuiltin, Builtin::Bool);
  llvm::sys::fs::remove_directories(Dir);
}

TEST(DWARFTypeImportGo, LegacyResultsKeepRealPointerInputs) {
  const bool Params[] = {true, true};
  auto Sections = goFunctionSections("func(*bool) bool", Params);
  auto Context = llvm::DWARFContext::create(Sections, 8, true);
  DWARFTypeImportStats Stats;
  auto Library = importDWARFTypes(*Context, DataModel::LP64, &Stats);
  ASSERT_TRUE(static_cast<bool>(Library))
      << llvm::toString(Library.takeError());
  EXPECT_TRUE(Stats.Skipped.empty());
  const LocalType *Callback = (*Library)->find("Callback");
  ASSERT_NE(Callback, nullptr);
  ASSERT_EQ(Callback->Target->Kind, TypeExprKind::Pointer);
  const TypeExpr &Function = *Callback->Target->Element;
  ASSERT_EQ(Function.Kind, TypeExprKind::Function);
  ASSERT_EQ(Function.Function->Params.size(), 1u);
  EXPECT_EQ(Function.Function->Params[0].Type->Kind, TypeExprKind::Pointer);
  EXPECT_EQ(Function.Function->Return->TheBuiltin, Builtin::Bool);
}

TEST(DWARFTypeImportGo, AnAmbiguousLegacyFunctionKeepsItsClosureAddress) {
  const bool Params[] = {false, true};
  for (const char *Name : {"main.Callback", ""}) {
    SCOPED_TRACE(Name);
    auto Sections = goFunctionSections(Name, Params);
    auto Context = llvm::DWARFContext::create(Sections, 8, true);
    DWARFTypeImportStats Stats;
    auto Library = importDWARFTypes(*Context, DataModel::LP64, &Stats);
    ASSERT_TRUE(static_cast<bool>(Library))
        << llvm::toString(Library.takeError());
    EXPECT_TRUE(Stats.Skipped.empty());
    const LocalType *Callback = (*Library)->find("Callback");
    ASSERT_NE(Callback, nullptr);
    ASSERT_EQ(Callback->Target->Kind, TypeExprKind::Pointer);
    ASSERT_EQ(Callback->Target->Element->Kind, TypeExprKind::Builtin);
    EXPECT_EQ(Callback->Target->Element->TheBuiltin, Builtin::Void);
    EXPECT_EQ((*Library)->sizeOf(*Callback->Target), 8u);
  }
}

TEST(DWARFTypeImportGo, ReportsMalformedLegacySignaturesAndResultTypes) {
  const struct {
    const char *Name;
    bool Params[2];
    unsigned Count;
    const char *Reason;
  } Cases[] = {
      {"func(bool", {false}, 1, "incomplete Go function signature"},
      {"func(,bool) bool",
       {false, true},
       2,
       "incomplete Go function signature"},
      {"func(struct { X string \"unterminated }) bool",
       {false, true},
       2,
       "incomplete Go function signature"},
      {"func(bool) bool",
       {false},
       1,
       "Go signature that disagrees with its parameters"},
      {"func(bool)",
       {false, true},
       2,
       "Go signature that disagrees with its parameters"},
      {"func(bool) bool",
       {false, false},
       2,
       "legacy Go result that is not a pointer"},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto Sections = goFunctionSections(
        Case.Name, llvm::ArrayRef(Case.Params).take_front(Case.Count));
    auto Context = llvm::DWARFContext::create(Sections, 8, true);
    DWARFTypeImportStats Stats;
    auto Library = importDWARFTypes(*Context, DataModel::LP64, &Stats);
    ASSERT_TRUE(static_cast<bool>(Library))
        << llvm::toString(Library.takeError());
    EXPECT_EQ((*Library)->find("Callback"), nullptr);
    ASSERT_FALSE(Stats.Skipped.empty());
    EXPECT_NE(Stats.Skipped.front().find(Case.Reason), std::string::npos)
        << Stats.Skipped.front();
  }
}

TEST(DWARFTypeImportGo, ExplicitInputMarkersKeepANamedVoidFunction) {
  const bool Params[] = {true};
  auto Sections = goFunctionSections("main.Callback", Params, true);
  auto Context = llvm::DWARFContext::create(Sections, 8, true);
  DWARFTypeImportStats Stats;
  auto Library = importDWARFTypes(*Context, DataModel::LP64, &Stats);
  ASSERT_TRUE(static_cast<bool>(Library))
      << llvm::toString(Library.takeError());
  EXPECT_TRUE(Stats.Skipped.empty());
  const LocalType *Callback = (*Library)->find("Callback");
  ASSERT_NE(Callback, nullptr);
  ASSERT_EQ(Callback->Target->Kind, TypeExprKind::Pointer);
  const TypeExpr &Function = *Callback->Target->Element;
  ASSERT_EQ(Function.Kind, TypeExprKind::Function);
  ASSERT_EQ(Function.Function->Params.size(), 1u);
  EXPECT_EQ(Function.Function->Params[0].Type->Kind, TypeExprKind::Pointer);
  EXPECT_EQ(Function.Function->Return->TheBuiltin, Builtin::Void);
}

TEST(DWARFTypeImportArray, CountsSignedBoundsWithoutOverflow) {
  constexpr int64_t Min = std::numeric_limits<int64_t>::min();
  constexpr int64_t Max = std::numeric_limits<int64_t>::max();
  const struct {
    int64_t Lower;
    int64_t Upper;
    std::optional<uint64_t> Count;
  } Cases[] = {{0, 3, 4},
               {-3, 2, 6},
               {0, Max, uint64_t(1) << 63},
               {Min, -1, uint64_t(1) << 63},
               {Min, Max - 1, std::numeric_limits<uint64_t>::max()},
               {Max, Max, 1},
               {1, 0, std::nullopt}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Lower);
    SCOPED_TRACE(Case.Upper);
    auto Sections = arrayBoundSections(Case.Lower, Case.Upper);
    auto Context = llvm::DWARFContext::create(Sections, 8, true);
    DWARFTypeImportStats Stats;
    auto Library = importDWARFTypes(*Context, DataModel::LP64, &Stats);
    ASSERT_TRUE(static_cast<bool>(Library))
        << llvm::toString(Library.takeError());
    EXPECT_TRUE(Stats.Skipped.empty());
    const LocalType *Array = (*Library)->find("BoundedArray");
    ASSERT_NE(Array, nullptr);
    ASSERT_TRUE(Array->Target);
    ASSERT_EQ(Array->Target->Kind, TypeExprKind::Array);
    EXPECT_EQ(Array->Target->Count, Case.Count);
  }
}

TEST(DWARFTypeImportArray, RefusesAnUnrepresentableFullSignedRange) {
  auto Sections = arrayBoundSections(std::numeric_limits<int64_t>::min(),
                                     std::numeric_limits<int64_t>::max());
  auto Context = llvm::DWARFContext::create(Sections, 8, true);
  DWARFTypeImportStats Stats;
  auto Library = importDWARFTypes(*Context, DataModel::LP64, &Stats);
  ASSERT_TRUE(static_cast<bool>(Library))
      << llvm::toString(Library.takeError());
  EXPECT_EQ((*Library)->find("BoundedArray"), nullptr);
  ASSERT_FALSE(Stats.Skipped.empty());
  EXPECT_NE(Stats.Skipped.front().find("array bound range that exceeds 64 bits"),
            std::string::npos);
}

