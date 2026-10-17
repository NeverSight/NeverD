//===- DWARFTypeImportTests.cpp - Local types from DWARF type entries -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "TypeShape.h"

#include "neverd/types/DWARFTypeImport.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <gtest/gtest.h>

using namespace neverd::types;
using neverd::types::test::shapeOf;

namespace {

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
  EXPECT_EQ(builtinBytes(Color->Enum->Underlying, DataModel::LP64), 4u);
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

TEST(DWARFTypeImportRust, ReadsRustsLayoutsAndLeavesVariantsOut) {
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
  // An enumeration with data is variants, which local types do not hold:
  // left out, with why; its variants' own structures stay.
  EXPECT_EQ(Library.find("rust_dwarf::Shape"), nullptr);
  ASSERT_EQ(Stats.Skipped.size(), 1u);
  EXPECT_EQ(Stats.Skipped[0], "rust_dwarf::Shape: variants, as a Rust "
                              "enumeration's");
  EXPECT_NE(Library.find("rust_dwarf::Shape::Square"), nullptr);
}

TEST(DWARFTypeImportGo, ReadsGosStructuresByTheirPackagesNames) {
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
              "//go:noinline\n"
              "func area(p *Point) int64 { return int64(p.X) * p.Y }\n"
              "func main() { println(area(&Point{X: 2, Y: 3})) }\n";
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
  llvm::sys::fs::remove_directories(Dir);
}
