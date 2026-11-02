//===- PDBTypeImportTests.cpp - Local types from PDB type records ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "TypeShape.h"

#include "neverd/types/CodeViewCallingConvention.h"
#include "neverd/types/PDBTypeImport.h"

#include <cstdlib>
#include <gtest/gtest.h>

using namespace neverd::types;
using neverd::types::test::shapeOf;

namespace {

std::unique_ptr<TypeLibrary> load(llvm::StringRef Path,
                                  PDBTypeImportStats &Stats) {
  auto Library = importPDBTypes(Path, &Stats);
  if (!Library) {
    ADD_FAILURE() << llvm::toString(Library.takeError());
    return nullptr;
  }
  return std::move(*Library);
}

const RecordDef &recordOf(const TypeLibrary &Library, llvm::StringRef Name) {
  const LocalType *Type = Library.find(Name);
  EXPECT_NE(Type, nullptr) << Name.str();
  static const RecordDef Empty;
  return Type && Type->Record ? *Type->Record : Empty;
}

/// The member \p Name of \p Record, searched through anonymous records,
/// with its offset from \p Record in bits.
const Member *memberNamed(const RecordDef &Record, llvm::StringRef Name,
                          uint64_t &Bits, uint64_t Base = 0) {
  for (const Member &Field : Record.Members) {
    if (Field.Name == Name) {
      Bits = Base + Field.BitOffset;
      return &Field;
    }
    if (Field.isAnonymous())
      if (const Member *Found = memberNamed(*Field.Type->Record, Name, Bits,
                                            Base + Field.BitOffset))
        return Found;
  }
  return nullptr;
}

class PDBTypeImportFixture : public ::testing::Test {
protected:
  void SetUp() override {
    Library = load(NEVERD_TYPES_FIXTURES "/local_types.pdb", Stats);
    ASSERT_NE(Library, nullptr);
  }

  PDBTypeImportStats Stats;
  std::unique_ptr<TypeLibrary> Library;
};

} // namespace

TEST(PDBTypeImport, CodeViewCallbackConventionsFollowTheTargetWidth) {
  using CC = llvm::codeview::CallingConvention;
  EXPECT_EQ(codeViewCallingConvention(CC::NearStdCall, 4),
            CallingConvention::Stdcall);
  EXPECT_EQ(codeViewCallingConvention(CC::NearFast, 4),
            CallingConvention::Fastcall);
  EXPECT_EQ(codeViewCallingConvention(CC::ThisCall, 4),
            CallingConvention::Thiscall);
  for (auto Convention :
       {CC::NearC, CC::NearStdCall, CC::NearFast, CC::ThisCall})
    EXPECT_EQ(codeViewCallingConvention(Convention, 8),
              CallingConvention::Unspecified);
  for (unsigned Width : {4u, 8u}) {
    EXPECT_EQ(codeViewCallingConvention(CC::NearVector, Width),
              CallingConvention::Vectorcall);
    EXPECT_FALSE(codeViewCallingConvention(CC::NearPascal, Width));
  }
}

TEST_F(PDBTypeImportFixture, ReadsEveryNamedType) {
  EXPECT_EQ(Library->dataModel(), DataModel::LLP64);
  EXPECT_TRUE(Stats.NestingMismatches.empty());
  EXPECT_EQ(Stats.UnsupportedFunctionTypes, 0u);
  for (const char *Name :
       {"_LIST_ENTRY", "_LARGE_INTEGER", "_PUSH_LOCK", "_THREAD_LIKE",
        "_DEVICE", "_PACKED", "_USES_FORWARD", "_POOL_KIND", "_SIGNED_KIND"})
    EXPECT_NE(Library->find(Name), nullptr) << Name;
  // Records named only by their place are defined where they are used.
  for (const LocalType *Type : Library->types())
    EXPECT_EQ(Type->Name.find("<unnamed"), std::string::npos) << Type->Name;
  EXPECT_EQ(Library->find("_LARGE_INTEGER")->Kind, TypeKind::Union);
}

TEST_F(PDBTypeImportFixture, RebuildsAnonymousUnionsAndStructures) {
  EXPECT_EQ(shapeOf(recordOf(*Library, "_LARGE_INTEGER")),
            "union{struct@0{LowPart@0 HighPart@4} "
            "u@0:struct{LowPart@0 HighPart@4} QuadPart@0}");
  EXPECT_EQ(shapeOf(recordOf(*Library, "_PUSH_LOCK")),
            "struct{union@0{struct@0{Locked@0b:1 Waiting@1b:1 Waking@2b:1 "
            "MultipleShared@3b:1 Shared@4b:60} Value@0 Ptr@0}}");
}

TEST_F(PDBTypeImportFixture, KeepsSiblingFlagUnionsApart) {
  EXPECT_EQ(shapeOf(recordOf(*Library, "_THREAD_LIKE")),
            "struct{Header@0 "
            "union@8{struct@0{AutoBoostActive@0b:1 ReadyTransition@1b:1 "
            "MiscSpare@2b:30} MiscFlags@0} "
            "union@12{struct@0{ThreadFlagsSpare@0b:2 AutoAlignment@2b:1 "
            "DisableBoost@3b:1 ThreadReserved@4b:28} ThreadFlags@0} Tail@16}");
  EXPECT_EQ(recordOf(*Library, "_THREAD_LIKE").Size, 24u);
}

TEST_F(PDBTypeImportFixture, KeepsOffsetsSizesAndMemberTypes) {
  const RecordDef &Device = recordOf(*Library, "_DEVICE");
  EXPECT_EQ(Device.Size, 88u);
  EXPECT_EQ(shapeOf(Device), "struct{Type@0 Size@2 Links@8 MajorFunction@24 "
                             "Next@56 Time@64 Lock@72 Pool@80}");
  uint64_t Bits = 0;
  const Member *Dispatch = memberNamed(Device, "MajorFunction", Bits);
  ASSERT_NE(Dispatch, nullptr);
  ASSERT_EQ(Dispatch->Type->Kind, TypeExprKind::Array);
  EXPECT_EQ(Dispatch->Type->Count, std::optional<uint64_t>(4));
  const TypeExpr &Pointer = *Dispatch->Type->Element;
  ASSERT_EQ(Pointer.Kind, TypeExprKind::Pointer);
  ASSERT_EQ(Pointer.Element->Kind, TypeExprKind::Function);
  const FunctionDef &Function = *Pointer.Element->Function;
  ASSERT_EQ(Function.Params.size(), 2u);
  EXPECT_EQ(Function.Params[0].Type->Kind, TypeExprKind::Pointer);
  EXPECT_EQ(Function.Params[0].Type->Element->Name, "_DEVICE");
  EXPECT_EQ(Function.Return->TheBuiltin, Builtin::Long);
  EXPECT_EQ(Function.Convention, CallingConvention::Unspecified);

  const Member *Pool = memberNamed(Device, "Pool", Bits);
  ASSERT_NE(Pool, nullptr);
  EXPECT_EQ(Pool->Type->Kind, TypeExprKind::Named);
  EXPECT_EQ(Pool->Type->NamedKind, TypeKind::Enum);
  EXPECT_EQ(Library->sizeOf(*Pool->Type), 4u);
}

TEST_F(PDBTypeImportFixture, ReadsEnumerations) {
  const LocalType *Pool = Library->find("_POOL_KIND");
  ASSERT_NE(Pool, nullptr);
  ASSERT_EQ(Pool->Enum->Values.size(), 3u);
  EXPECT_EQ(Pool->Enum->Values[2].Name, "MustSucceed");
  EXPECT_EQ(static_cast<uint32_t>(Pool->Enum->Values[2].Value), 0x80000000u);
  const LocalType *Signed = Library->find("_SIGNED_KIND");
  ASSERT_NE(Signed, nullptr);
  EXPECT_EQ(static_cast<int64_t>(Signed->Enum->Values[0].Value), -1);
  EXPECT_EQ(Signed->Enum->Values[2].Value, 1u);
}

TEST_F(PDBTypeImportFixture, InfersPackingAndKeepsDeclarations) {
  const RecordDef &Packed = recordOf(*Library, "_PACKED");
  EXPECT_EQ(Packed.Size, 7u);
  EXPECT_EQ(Packed.Pack, 1u);
  EXPECT_EQ(recordOf(*Library, "_DEVICE").Pack, 0u);
  const LocalType *Forward = Library->find("_FORWARD_ONLY");
  ASSERT_NE(Forward, nullptr);
  EXPECT_FALSE(Forward->Record->Defined);
  EXPECT_EQ(Stats.DeclaredOnly, 1u);
}

/// The Windows kernel's own PDB, when NEVERD_TYPES_NTKRNLMP names a copy of
/// ntkrnlmp.pdb for 10.0.19041.6456; it is too large to keep in the tree.
TEST(PDBTypeImportKernel, RebuildsTheKernelTypes) {
  const char *Path = std::getenv("NEVERD_TYPES_NTKRNLMP");
  if (!Path)
    GTEST_SKIP() << "NEVERD_TYPES_NTKRNLMP names no ntkrnlmp.pdb";
  PDBTypeImportStats Stats;
  auto Library = load(Path, Stats);
  ASSERT_NE(Library, nullptr);
  EXPECT_TRUE(Stats.NestingMismatches.empty())
      << Stats.NestingMismatches.size() << " records, first "
      << Stats.NestingMismatches.front();
  ::testing::Test::RecordProperty("structs", Stats.Structs);
  ::testing::Test::RecordProperty("unions", Stats.Unions);
  ::testing::Test::RecordProperty("enums", Stats.Enums);
  ::testing::Test::RecordProperty("declared", Stats.DeclaredOnly);

  const RecordDef &Process = recordOf(*Library, "_EPROCESS");
  EXPECT_EQ(Process.Size, 0xA40u);
  uint64_t Bits = 0;
  ASSERT_NE(memberNamed(Process, "UniqueProcessId", Bits), nullptr);
  EXPECT_EQ(Bits / 8, 0x440u);
  ASSERT_NE(memberNamed(Process, "Flags2", Bits), nullptr);
  EXPECT_EQ(Bits / 8, 0x460u);
  ASSERT_NE(memberNamed(Process, "JobNotReallyActive", Bits), nullptr);
  EXPECT_EQ(Bits, 0x460u * 8);

  // _KTHREAD keeps MiscFlags and ThreadFlags in unions of their own.
  const RecordDef &Thread = recordOf(*Library, "_KTHREAD");
  const Member *Misc = nullptr;
  const Member *Flags = nullptr;
  for (const Member &Field : Thread.Members) {
    if (!Field.isAnonymous())
      continue;
    uint64_t Inner = 0;
    if (memberNamed(*Field.Type->Record, "MiscFlags", Inner))
      Misc = &Field;
    if (memberNamed(*Field.Type->Record, "ThreadFlags", Inner))
      Flags = &Field;
  }
  ASSERT_NE(Misc, nullptr);
  ASSERT_NE(Flags, nullptr);
  EXPECT_NE(Misc, Flags);
  EXPECT_EQ(Misc->BitOffset / 8, 0x74u);
  EXPECT_EQ(Flags->BitOffset / 8, 0x78u);
  uint64_t Spare = 0;
  EXPECT_NE(memberNamed(*Flags->Type->Record, "ThreadFlagsSpare", Spare),
            nullptr);
}
