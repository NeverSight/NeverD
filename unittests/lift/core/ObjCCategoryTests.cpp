#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCMethods.h"
#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstring>

using namespace neverd;

namespace {
class CategoryImage {
public:
  BinaryImage Image;

  void pointer(va_t Address, va_t Value) {
    auto &Segment = Image.Segments[Address >= 0x2000 ? 1 : 0];
    llvm::support::endian::write64le(Segment.Data.data() + Address - Segment.VA,
                                     Value);
    Image.MachOResolvedChainedPointerSlots.insert(Address);
  }
  void word(va_t Address, uint32_t Value) {
    auto &Segment = Image.Segments[Address >= 0x2000 ? 1 : 0];
    llvm::support::endian::write32le(Segment.Data.data() + Address - Segment.VA,
                                     Value);
  }
  void string(va_t Address, const char *Value) {
    std::memcpy(Image.Segments[1].Data.data() + Address - 0x2000, Value,
                std::strlen(Value) + 1);
  }
  void methodList(va_t List, va_t Selector, va_t Types, va_t Implementation) {
    word(List, 24);
    word(List + 4, 1);
    pointer(List + 8, Selector);
    pointer(List + 16, Types);
    pointer(List + 24, Implementation);
  }
  bool diagnostic(llvm::StringRef Text) const {
    return std::any_of(Image.ObjCMetadataDiagnostics.begin(),
                       Image.ObjCMetadataDiagnostics.end(),
                       [&](const std::string &D) {
                         return llvm::StringRef(D).contains(Text);
                       });
  }

  CategoryImage() {
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.Bits = Bitness::Bits64;
    for (unsigned I = 0; I < 2; ++I) {
      Segment Segment;
      Segment.VA = 0x1000 + I * 0x1000;
      Segment.FileOff = I * 0x1000;
      Segment.Size = Segment.FileSz = I ? 0x2000 : 0x1000;
      Segment.Flags = SegmentFlags::Readable;
      if (!I)
        Segment.Flags = Segment.Flags | SegmentFlags::Executable;
      Segment.Data.resize(Segment.Size);
      Image.Segments.push_back(Segment);
    }
    auto Section = [&](const char *Name, va_t Address, uint64_t Size,
                       bool Executable = false) {
      neverd::Section Sec;
      Sec.Name = Name;
      Sec.VA = Address;
      Sec.FileOff = Address - 0x1000;
      Sec.Size = Sec.FileSz = Size;
      Sec.Flags = SegmentFlags::Readable;
      if (Executable) {
        Sec.Flags = Sec.Flags | SegmentFlags::Executable;
        Sec.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      }
      Image.Sections.push_back(Sec);
    };
    Section("__text", 0x1000, 0x1000, true);
    Section("__objc_classlist", 0x2000, 8);
    Section("__objc_catlist", 0x2010, 8);
    Section("__objc_nlcatlist", 0x2018, 8);
    Section("__objc_const", 0x2100, 0x1f00);
    pointer(0x2000, 0x2200);
    pointer(0x2010, 0x2400);
    pointer(0x2018, 0x2400); // +load list repeats the same category
    pointer(0x2200, 0x2240); // class isa
    pointer(0x2220, 0x2300); // class ro
    pointer(0x2240, 0x2240); // root metaclass isa
    pointer(0x2260, 0x2380); // metaclass ro
    word(0x2300, 2);         // RO_ROOT
    word(0x2304, 8);
    word(0x2308, 8);
    pointer(0x2318, 0x2500);
    word(0x2380, 3); // RO_ROOT | RO_META
    pointer(0x2398, 0x2500);
    pointer(0x2400, 0x2520); // category name
    pointer(0x2408, 0x2200); // owning class, never its metaclass
    pointer(0x2410, 0x2600); // instance methods
    pointer(0x2418, 0x2640); // class methods
    string(0x2500, "Container");
    string(0x2520, "Extra");
    string(0x2540, "step:");
    string(0x2560, "classStep:");
    string(0x2580, "i20@0:8i16");
    string(0x25a0, "i20#0:8i16");
    methodList(0x2600, 0x2540, 0x2580, 0x1100);
    methodList(0x2640, 0x2560, 0x25a0, 0x1110);
    word(0x1100, 0xd65f03c0);
    word(0x1110, 0xd65f03c0);
  }
};

TEST(ObjCCategories, RecoversInstanceAndClassMethodsWithCategoryIdentityOnce) {
  CategoryImage Fixture;
  parseObjCMethods(Fixture.Image);
  ASSERT_EQ(Fixture.Image.ObjCMethods.size(), 2U);
  for (size_t I = 0; I < 2; ++I) {
    const auto &Method = Fixture.Image.ObjCMethods[I];
    EXPECT_EQ(Method.ClassName, "Container");
    EXPECT_EQ(Method.ClassAddress, 0x2200U);
    EXPECT_EQ(Method.CategoryName, "Extra");
    EXPECT_EQ(Method.CategoryAddress, 0x2400U);
    EXPECT_EQ(Method.IsClassMethod, I == 1);
    EXPECT_EQ(Method.Status, "supported");
    ASSERT_TRUE(Method.TypeHint);
    EXPECT_EQ(Method.TypeHint->Parameters.size(), 3U);
  }
  parseObjCMethods(Fixture.Image);
  EXPECT_EQ(Fixture.Image.ObjCMethods.size(), 2U);
}

TEST(ObjCCategories, AuthenticatedMetadataRecoversMethodsWithoutPlainPointers) {
  for (bool Chained : {false, true}) {
    CategoryImage Fixture;
    auto &Image = Fixture.Image;
    Image.MachOCPUSubtype = 0x80000002;
    Image.MachOHasChainedFixups = Chained;
    const auto Slots = Image.MachOResolvedChainedPointerSlots;
    for (va_t Slot : Slots) {
      const uint64_t Target =
          llvm::support::endian::read64le(Image.readVA(Slot, 8));
      const uint64_t Recipe = 0x8005432100000000ULL | Target;
      Fixture.pointer(Slot, Recipe);
      MachOAuthenticatedPointer Auth;
      Auth.Target = Target;
      Auth.EncodedValue = Recipe;
      Auth.Key = 2;
      Auth.Diversity = 0x4321;
      Auth.AddressDiversity = true;
      Image.MachOAuthenticatedPointers.emplace(Slot, Auth);
    }
    Image.MachOResolvedChainedPointerSlots.clear();
    parseObjCMethods(Image);
    ASSERT_EQ(Image.ObjCClasses.size(), 1);
    ASSERT_EQ(Image.ObjCMethods.size(), 2);
    for (size_t I = 0; I < Image.ObjCMethods.size(); ++I) {
      const auto &Method = Image.ObjCMethods[I];
      EXPECT_EQ(Method.ClassName, "Container");
      EXPECT_EQ(Method.CategoryName, "Extra");
      EXPECT_EQ(Method.Status, "supported");
      EXPECT_EQ(Method.Implementation, I ? 0x1110 : 0x1100);
    }
    EXPECT_TRUE(Image.MachOResolvedChainedPointerSlots.empty());
    for (const auto &[Slot, Auth] : Image.MachOAuthenticatedPointers) {
      EXPECT_EQ(llvm::support::endian::read64le(Image.readVA(Slot, 8)),
                *Auth.EncodedValue);
      EXPECT_FALSE(Image.CodePtrRelocSlots.count(Slot));
      EXPECT_FALSE(Image.DataPtrRelocSlots.count(Slot));
    }
    // The method's raw bytes must still match the loader's signing recipe.
    Fixture.pointer(0x2618, 0x1100);
    Image.MachOResolvedChainedPointerSlots.clear();
    parseObjCMethods(Image);
    ASSERT_EQ(Image.ObjCMethods.size(), 2);
    EXPECT_EQ(Image.ObjCMethods[0].Status, "invalid_implementation");
    EXPECT_EQ(Image.ObjCMethods[1].Status, "supported");
  }
}

TEST(ObjCCategories, NamesImplementationsAsClangDoes) {
  // A stripped image's implementations take the names clang gives them.
  CategoryImage Fixture;
  parseObjCMethods(Fixture.Image);
  const Symbol *Instance = Fixture.Image.findSymbolAt(0x1100);
  const Symbol *ClassMethod = Fixture.Image.findSymbolAt(0x1110);
  ASSERT_TRUE(Instance && ClassMethod);
  EXPECT_EQ(Instance->Name, "-[Container(Extra) step:]");
  EXPECT_EQ(ClassMethod->Name, "+[Container(Extra) classStep:]");

  // A selector that is no Objective-C name keeps the address name.
  CategoryImage Hostile;
  Hostile.string(0x2540, "s(){x;}:");
  parseObjCMethods(Hostile.Image);
  const Symbol *Placeholder = Hostile.Image.findSymbolAt(0x1100);
  ASSERT_TRUE(Placeholder);
  EXPECT_EQ(Placeholder->Name, "objc_imp_1100");
}

TEST(ObjCCategories, MetadataNamesReplaceOnlySynthesizedFunctionNames) {
  for (NameOrigin Origin : {NameOrigin::Synthesized, NameOrigin::Analysis,
                            NameOrigin::Stated, NameOrigin::User}) {
    CategoryImage Fixture;
    Symbol Existing = Symbol::makeFunc(0x1100, 16);
    Existing.Origin = Origin;
    Existing.IsBoundaryGuess = true;
    Fixture.Image.Symbols.push_back(Existing);
    parseObjCMethods(Fixture.Image);
    EXPECT_EQ(Fixture.Image.getFunctionNameAt(0x1100),
              Origin == NameOrigin::Synthesized ? "-[Container(Extra) step:]"
                                                : Existing.Name);
    EXPECT_EQ(Fixture.Image.Symbols.front().Size, 16U);
    EXPECT_EQ(Fixture.Image.Symbols.front().IsBoundaryGuess,
              Origin != NameOrigin::Synthesized);
    EXPECT_EQ(Fixture.Image.Symbols.size(), 2U);
    EXPECT_EQ(Fixture.Image.findSymbolAt(0x1100)->Name,
              Fixture.Image.getFunctionNameAt(0x1100));
    const size_t Count = Fixture.Image.Symbols.size();
    parseObjCMethods(Fixture.Image);
    EXPECT_EQ(Fixture.Image.Symbols.size(), Count);
  }
}

TEST(ObjCCategories, DiscoversLocalOwnerWithoutAClassListEntry) {
  CategoryImage Fixture;
  Fixture.pointer(0x2000, 0);
  parseObjCMethods(Fixture.Image);
  ASSERT_EQ(Fixture.Image.ObjCClasses.size(), 1U);
  EXPECT_EQ(Fixture.Image.ObjCMethods.size(), 2U);
}

TEST(ObjCCategories, RequiresResolvedChainedCategoryAndClassPointerSlots) {
  CategoryImage Fixture;
  Fixture.Image.MachOHasChainedFixups = true;
  parseObjCMethods(Fixture.Image);
  ASSERT_EQ(Fixture.Image.ObjCMethods.size(), 2U);
  Fixture.Image.MachOResolvedChainedPointerSlots.erase(0x2408);
  parseObjCMethods(Fixture.Image);
  EXPECT_TRUE(Fixture.Image.ObjCMethods.empty());
  EXPECT_TRUE(Fixture.diagnostic("target class is unresolved"));
  Fixture.Image.MachOResolvedChainedPointerSlots.insert(0x2408);
  Fixture.Image.MachOResolvedChainedPointerSlots.erase(0x2010);
  Fixture.Image.MachOResolvedChainedPointerSlots.erase(0x2018);
  parseObjCMethods(Fixture.Image);
  EXPECT_TRUE(Fixture.Image.ObjCMethods.empty());
  EXPECT_TRUE(Fixture.diagnostic("category address is unavailable"));
}

TEST(ObjCCategories,
     ResolvesExactImportedOwnerWithoutInventingLocalClassLayout) {
  CategoryImage Fixture;
  Fixture.pointer(0x2000, 0);
  Fixture.pointer(0x2408, 0);
  Fixture.Image.DyldBindSlots[0x2408] = {"_OBJC_CLASS_$_External", 0};
  Fixture.Image.MachOHasChainedFixups = true;
  parseObjCMethods(Fixture.Image);
  ASSERT_EQ(Fixture.Image.ObjCMethods.size(), 2U);
  EXPECT_TRUE(Fixture.Image.ObjCClasses.empty());
  EXPECT_EQ(Fixture.Image.ObjCMethods[0].ClassName, "External");
  EXPECT_EQ(Fixture.Image.ObjCMethods[0].ClassAddress, 0U);
  EXPECT_EQ(Fixture.Image.ObjCMethods[0].CategoryName, "Extra");
  Fixture.Image.DyldBindSlots[0x2408].Addend = 8;
  parseObjCMethods(Fixture.Image);
  EXPECT_TRUE(Fixture.Image.ObjCMethods.empty());
}

TEST(ObjCCategories, RejectsMetaclassAndConflictingImportedOwner) {
  CategoryImage Fixture;
  Fixture.pointer(0x2408, 0x2240);
  parseObjCMethods(Fixture.Image);
  EXPECT_TRUE(Fixture.Image.ObjCMethods.empty());
  Fixture.Image.DyldBindSlots[0x2408] = {"_OBJC_METACLASS_$_Container", 0};
  parseObjCMethods(Fixture.Image);
  EXPECT_TRUE(Fixture.Image.ObjCMethods.empty());
  Fixture.Image.DyldBindSlots[0x2408] = {"_OBJC_CLASS_$_Container", 0};
  Fixture.Image.ConflictingImportStorageSlots.insert(0x2408);
  parseObjCMethods(Fixture.Image);
  EXPECT_TRUE(Fixture.Image.ObjCMethods.empty());
  EXPECT_TRUE(Fixture.diagnostic("conflicting import bindings"));
}

void addCollidingCategory(CategoryImage &Fixture, va_t Types = 0x2580,
                          va_t Implementation = 0x1120) {
  Fixture.pointer(0x2018, 0x2440);
  Fixture.pointer(0x2440, 0x2528);
  Fixture.string(0x2528, "Another");
  Fixture.pointer(0x2448, 0x2200);
  Fixture.pointer(0x2450, 0x2680);
  Fixture.methodList(0x2680, 0x2540, Types, Implementation);
  Fixture.word(0x1120, 0xd65f03c0);
}

TEST(ObjCCategories, PreservesCollidingCategoryRecordsAndRejectsDispatchOrder) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool SharedImplementation : {false, true})
      for (bool Reverse : {false, true}) {
        CategoryImage Fixture;
        Fixture.Image.Arch = Architecture;
        addCollidingCategory(Fixture, 0x2580,
                             SharedImplementation ? 0x1100 : 0x1120);
        if (Reverse) {
          Fixture.pointer(0x2010, 0x2440);
          Fixture.pointer(0x2018, 0x2400);
        }
        parseObjCMethods(Fixture.Image);
        ASSERT_EQ(Fixture.Image.ObjCMethods.size(), 3U);
        unsigned Ambiguous = 0;
        std::set<std::string> Categories;
        for (const auto &Method : Fixture.Image.ObjCMethods)
          if (Method.Selector == "step:") {
            ++Ambiguous;
            EXPECT_EQ(Method.Status, "ambiguous_dispatch");
            EXPECT_TRUE(Method.TypeHint);
            Categories.insert(Method.CategoryName);
          }
        EXPECT_EQ(Ambiguous, 2U);
        EXPECT_EQ(Categories, (std::set<std::string>{"Extra", "Another"}));
        const auto Call = objcSelectorSourceTypeHint(Fixture.Image, "step:");
        ASSERT_TRUE(Call);
        EXPECT_EQ(Call->ReturnType->Size, 4U);
        EXPECT_EQ(Call->Parameters.size(), 3U);
        EXPECT_EQ(Call->Architecture, Architecture);
      }
}

TEST(ObjCCategories, CollidingDeclarationsStillVetoIncompatibleCallABIs) {
  for (const char *Encoding :
       {"q20@0:8i16", "i24@0:8d16", "{Unsupported=ii}20@0:8i16"})
    for (bool Reverse : {false, true}) {
      CategoryImage Fixture;
      Fixture.string(0x25c0, Encoding);
      addCollidingCategory(Fixture, 0x25c0);
      if (Reverse) {
        Fixture.pointer(0x2010, 0x2440);
        Fixture.pointer(0x2018, 0x2400);
      }
      parseObjCMethods(Fixture.Image);
      ASSERT_EQ(Fixture.Image.ObjCMethods.size(), 3U);
      EXPECT_FALSE(objcSelectorSourceTypeHint(Fixture.Image, "step:"));
    }
}

TEST(ObjCCategories, SharedImplementationConflictsStillInvalidateCallFacts) {
  CategoryImage Fixture;
  addCollidingCategory(Fixture);
  Fixture.string(0x25c0, "other:");
  Fixture.string(0x25e0, "v20@0:8i16");
  Fixture.word(0x2684, 2);
  Fixture.pointer(0x26a0, 0x25c0);
  Fixture.pointer(0x26a8, 0x25e0);
  Fixture.pointer(0x26b0, 0x1120);
  parseObjCMethods(Fixture.Image);
  ASSERT_EQ(Fixture.Image.ObjCMethods.size(), 4U);
  for (const auto &Method : Fixture.Image.ObjCMethods)
    if (Method.Implementation == 0x1120) {
      EXPECT_EQ(Method.Status, "conflicting_encoding");
      EXPECT_FALSE(Method.TypeHint);
    }
  EXPECT_FALSE(objcSelectorSourceTypeHint(Fixture.Image, "step:"));
  EXPECT_FALSE(objcSelectorSourceTypeHint(Fixture.Image, "other:"));
}

TEST(ObjCCategories, RejectsTruncatedPrefixAndMalformedMethodList) {
  CategoryImage Fixture;
  Fixture.pointer(0x2010, 0x3fe0);
  Fixture.pointer(0x2018, 0x3fe0);
  parseObjCMethods(Fixture.Image);
  EXPECT_TRUE(Fixture.Image.ObjCMethods.empty());
  EXPECT_TRUE(Fixture.diagnostic("Truncated"));
  Fixture.pointer(0x2010, 0x2400);
  Fixture.pointer(0x2018, 0x2400);
  Fixture.word(0x2604, 0xffffffffU);
  parseObjCMethods(Fixture.Image);
  ASSERT_EQ(Fixture.Image.ObjCMethods.size(), 1U);
  EXPECT_TRUE(Fixture.Image.ObjCMethods[0].IsClassMethod);
  EXPECT_TRUE(Fixture.diagnostic("excessive"));
}

TEST(ObjCCategories, AcceptsOnlyTheCommonPrefixWithoutReadingAnOptionalTail) {
  CategoryImage Fixture;
  // Place the exact six-pointer prefix at the end of the mapped section.
  const uint8_t *Original = Fixture.Image.readVA(0x2400, 48);
  std::memcpy(Fixture.Image.Segments[1].Data.data() + 0x1fd0, Original, 48);
  Fixture.pointer(0x2010, 0x3fd0);
  Fixture.pointer(0x2018, 0x3fd0);
  parseObjCMethods(Fixture.Image);
  ASSERT_EQ(Fixture.Image.ObjCMethods.size(), 2U);
  EXPECT_EQ(Fixture.Image.ObjCMethods[0].CategoryAddress, 0x3fd0U);
}
} // namespace
