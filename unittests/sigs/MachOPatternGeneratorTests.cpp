//===- MachOPatternGeneratorTests.cpp - Lines from Mach-O objects ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/PatternGenerator.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/MC/MCLinkerOptimizationHint.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace neverd::sigs;
using namespace llvm;

namespace {

/// A 64-bit Mach-O relocatable object with one `__text` section.
class MachOObjectBuilder {
public:
  MachOObjectBuilder(uint32_t CPUType, std::vector<uint8_t> Code)
      : CPUType(CPUType), Code(std::move(Code)) {}

  /// Places `__text` at \p Address of the object's address space, in which
  /// symbols and linker optimization hints state addresses; a relocation
  /// states an offset in the section.
  void setTextAddress(uint64_t Address) { TextAddress = Address; }

  /// A symbol defined in `__text` at \p Offset.
  void addSymbol(StringRef Name, uint64_t Offset) {
    Symbols.push_back({Name.str(), Offset, true});
  }
  /// An undefined symbol the section refers to.
  void addUndefined(StringRef Name) {
    Symbols.push_back({Name.str(), 0, false});
  }

  /// A relocation at \p Offset naming \p Symbol, or the section when it is
  /// empty.
  void addRelocation(uint64_t Offset, uint32_t Type, unsigned Length,
                     StringRef Symbol = "", bool Scattered = false) {
    Relocations.push_back({Offset, Type, Length, Symbol.str(), Scattered});
  }

  /// A linker optimization hint of \p Kind naming the instructions at
  /// \p Offsets in `__text`.
  void addHint(uint32_t Kind, std::vector<uint64_t> Offsets) {
    Hints.push_back({Kind, std::move(Offsets)});
  }
  /// Hint data as it stands, in place of the hints addHint encodes.
  void setHintData(std::vector<uint8_t> Data) { RawHints = std::move(Data); }

  std::vector<uint8_t> build() const {
    std::vector<uint8_t> Out;
    auto put = [&](uint64_t Value, unsigned Bytes) {
      for (unsigned I = 0; I < Bytes; ++I)
        Out.push_back(static_cast<uint8_t>(Value >> (8 * I)));
    };
    auto putName = [&](StringRef Name, size_t Bytes) {
      for (size_t I = 0; I < Bytes; ++I)
        Out.push_back(I < Name.size() ? Name[I] : 0);
    };

    // The assembler pads the hints to eight bytes with kind 0.
    std::vector<uint8_t> HintData;
    if (RawHints) {
      HintData = *RawHints;
    } else if (!Hints.empty()) {
      for (const Hint &H : Hints) {
        putULEB128(HintData, H.Kind);
        putULEB128(HintData, H.Offsets.size());
        for (uint64_t Offset : H.Offsets)
          putULEB128(HintData, TextAddress + Offset);
      }
      HintData.resize(alignTo(HintData.size(), 8), 0);
    }
    const bool HasHints = RawHints || !Hints.empty();

    const uint32_t HeaderSize = sizeof(MachO::mach_header_64);
    const uint32_t SegmentSize =
        sizeof(MachO::segment_command_64) + sizeof(MachO::section_64);
    const uint32_t SymtabSize = sizeof(MachO::symtab_command);
    const uint32_t HintCommandSize =
        HasHints ? sizeof(MachO::linkedit_data_command) : 0;
    const uint32_t CodeOffset =
        HeaderSize + SegmentSize + SymtabSize + HintCommandSize;
    const uint32_t RelocationOffset = CodeOffset + Code.size();
    const uint32_t SymbolOffset =
        RelocationOffset +
        Relocations.size() * sizeof(MachO::any_relocation_info);
    std::string Strings(1, '\0');
    std::vector<uint32_t> NameOffsets;
    for (const Symbol &Sym : Symbols) {
      NameOffsets.push_back(Strings.size());
      Strings += Sym.Name;
      Strings.push_back('\0');
    }
    const uint32_t StringOffset =
        SymbolOffset + Symbols.size() * sizeof(MachO::nlist_64);
    const uint32_t HintOffset = StringOffset + Strings.size();

    put(MachO::MH_MAGIC_64, 4);
    put(CPUType, 4);
    put(0, 4);
    put(MachO::MH_OBJECT, 4);
    put(HasHints ? 3 : 2, 4);
    put(SegmentSize + SymtabSize + HintCommandSize, 4);
    put(MachO::MH_SUBSECTIONS_VIA_SYMBOLS, 4);
    put(0, 4);

    put(MachO::LC_SEGMENT_64, 4);
    put(SegmentSize, 4);
    putName("", 16);
    put(TextAddress, 8);
    put(Code.size(), 8);
    put(CodeOffset, 8);
    put(Code.size(), 8);
    put(7, 4);
    put(7, 4);
    put(1, 4);
    put(0, 4);
    putName("__text", 16);
    putName("__TEXT", 16);
    put(TextAddress, 8);
    put(Code.size(), 8);
    put(CodeOffset, 4);
    put(2, 4);
    put(RelocationOffset, 4);
    put(Relocations.size(), 4);
    put(MachO::S_ATTR_PURE_INSTRUCTIONS | MachO::S_ATTR_SOME_INSTRUCTIONS, 4);
    put(0, 4);
    put(0, 4);
    put(0, 4);

    put(MachO::LC_SYMTAB, 4);
    put(SymtabSize, 4);
    put(SymbolOffset, 4);
    put(Symbols.size(), 4);
    put(StringOffset, 4);
    put(Strings.size(), 4);

    if (HasHints) {
      put(MachO::LC_LINKER_OPTIMIZATION_HINT, 4);
      put(HintCommandSize, 4);
      put(HintOffset, 4);
      put(HintData.size(), 4);
    }

    Out.insert(Out.end(), Code.begin(), Code.end());
    for (const Relocation &Rel : Relocations) {
      uint32_t SymbolNum = 1; // the section, when no symbol is named
      bool Extern = false;
      for (size_t I = 0; I < Symbols.size(); ++I)
        if (!Rel.Symbol.empty() && Symbols[I].Name == Rel.Symbol) {
          SymbolNum = I;
          Extern = true;
        }
      if (Rel.Scattered) {
        put(MachO::R_SCATTERED | (uint32_t(Rel.Type) << 24) |
                (uint32_t(Rel.Length) << 28) | Rel.Offset,
            4);
        put(0, 4);
        continue;
      }
      put(Rel.Offset, 4);
      put(SymbolNum | (uint32_t(Rel.Length) << 25) | (uint32_t(Extern) << 27) |
              (uint32_t(Rel.Type) << 28),
          4);
    }
    for (size_t I = 0; I < Symbols.size(); ++I) {
      put(NameOffsets[I], 4);
      const uint8_t Type = Symbols[I].Defined ? MachO::N_SECT : MachO::N_UNDF;
      Out.push_back(Type | MachO::N_EXT);
      Out.push_back(Symbols[I].Defined ? 1 : 0);
      put(0, 2);
      put(Symbols[I].Defined ? TextAddress + Symbols[I].Offset : 0, 8);
    }
    Out.insert(Out.end(), Strings.begin(), Strings.end());
    Out.insert(Out.end(), HintData.begin(), HintData.end());
    return Out;
  }

private:
  struct Symbol {
    std::string Name;
    uint64_t Offset;
    bool Defined;
  };
  struct Relocation {
    uint64_t Offset;
    uint32_t Type;
    unsigned Length;
    std::string Symbol;
    bool Scattered;
  };
  struct Hint {
    uint32_t Kind;
    std::vector<uint64_t> Offsets;
  };

  static void putULEB128(std::vector<uint8_t> &Out, uint64_t Value) {
    uint8_t Bytes[16];
    const unsigned Size = encodeULEB128(Value, Bytes);
    Out.insert(Out.end(), Bytes, Bytes + Size);
  }

  uint32_t CPUType;
  std::vector<uint8_t> Code;
  uint64_t TextAddress = 0;
  std::vector<Symbol> Symbols;
  std::vector<Relocation> Relocations;
  std::vector<Hint> Hints;
  std::optional<std::vector<uint8_t>> RawHints;
};

std::vector<uint8_t> sequentialCode(size_t Size) {
  std::vector<uint8_t> Code(Size);
  for (size_t I = 0; I < Size; ++I)
    Code[I] = static_cast<uint8_t>(0x40 + I);
  return Code;
}

struct Generated {
  std::vector<std::string> Lines;
  PatternGeneratorStats Stats;
};

std::unique_ptr<object::ObjectFile> open(const std::vector<uint8_t> &Bytes) {
  Expected<std::unique_ptr<object::ObjectFile>> Obj =
      object::ObjectFile::createObjectFile(MemoryBufferRef(
          StringRef(reinterpret_cast<const char *>(Bytes.data()), Bytes.size()),
          "test.o"));
  if (!Obj) {
    ADD_FAILURE() << toString(Obj.takeError());
    return nullptr;
  }
  return std::move(*Obj);
}

Generated generate(const std::vector<uint8_t> &Bytes, bool Evidence = false) {
  Generated Out;
  std::unique_ptr<object::ObjectFile> Obj = open(Bytes);
  if (!Obj)
    return Out;
  PatternGeneratorOptions Opts;
  Opts.TailLen = 0xFFFF;
  Opts.EmitReferences = true;
  Opts.CollisionEvidence = Evidence;
  std::string Text;
  raw_string_ostream OS(Text);
  Out.Stats = generatePatterns(*Obj, Opts, OS);
  OS.flush();
  StringRef Rest = Text;
  while (!Rest.empty()) {
    auto [Line, Tail] = Rest.split('\n');
    Out.Lines.push_back(Line.str());
    Rest = Tail;
  }
  return Out;
}

TEST(MachOPatternGenerator, CoversEachARM64RelocationsBytes) {
  MachOObjectBuilder Builder(MachO::CPU_TYPE_ARM64, sequentialCode(64));
  Builder.addSymbol("_caller", 0);
  Builder.addUndefined("_malloc");
  Builder.addUndefined("_table");
  Builder.addRelocation(8, MachO::ARM64_RELOC_BRANCH26, 2, "_malloc");
  Builder.addRelocation(12, MachO::ARM64_RELOC_PAGE21, 2, "_table");
  // An addend, at the address of the PAGEOFF12 it belongs to.
  Builder.addRelocation(16, MachO::ARM64_RELOC_ADDEND, 2);
  Builder.addRelocation(16, MachO::ARM64_RELOC_PAGEOFF12, 2, "_table");
  // An eight-byte pointer.
  Builder.addRelocation(40, MachO::ARM64_RELOC_UNSIGNED, 3);

  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0])
                  .starts_with("4041424344454647........................"
                               "5455565758595A5B5C5D5E5F"));
  EXPECT_NE(Out.Lines[0].find(" :0000 _caller ^0008 _malloc "),
            std::string::npos);
  // The CRC span runs from the leading bytes to the pointer; the tail states
  // the pointer's eight bytes as wildcards and the rest exactly.
  EXPECT_TRUE(StringRef(Out.Lines[0])
                  .ends_with(" ................707172737475767778797A7B7C7D7E"
                             "7F"));
}

TEST(MachOPatternGenerator, NamesAnAddressesAliasesOnOneLine) {
  MachOObjectBuilder Builder(MachO::CPU_TYPE_ARM64, sequentialCode(64));
  Builder.addSymbol("__compress", 0);
  Builder.addSymbol("_compress", 0);
  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_NE(Out.Lines[0].find(" :0000 _compress :0000 __compress"),
            std::string::npos);
}

TEST(MachOPatternGenerator, LeavesOutAFunctionWithAnUnknownRelocation) {
  MachOObjectBuilder Builder(MachO::CPU_TYPE_ARM64, sequentialCode(96));
  Builder.addSymbol("_kept", 0);
  Builder.addSymbol("_unknown", 32);
  Builder.addSymbol("_scattered", 64);
  Builder.addRelocation(36, 12, 2);
  Builder.addRelocation(68, MachO::ARM64_RELOC_PAGE21, 2, "",
                        /*Scattered=*/true);
  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).contains(":0000 _kept"));
  EXPECT_EQ(Out.Stats.UnsupportedRelocation, 2u);
  EXPECT_EQ(
      Out.Stats.UnsupportedMachORelocations.count({MachO::CPU_TYPE_ARM64, 12}),
      1u);
}

TEST(MachOPatternGenerator, LeavesOutRelocatedFunctionsOfOtherCPUTypes) {
  MachOObjectBuilder Builder(MachO::CPU_TYPE_ARM, sequentialCode(64));
  Builder.addSymbol("_relocated", 0);
  Builder.addSymbol("_plain", 32);
  Builder.addRelocation(4, MachO::ARM_RELOC_VANILLA, 2);
  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).contains(":0000 _plain"));
  EXPECT_EQ(Out.Stats.UnsupportedMachORelocations.count(
                {MachO::CPU_TYPE_ARM, MachO::ARM_RELOC_VANILLA}),
            1u);
}

TEST(MachOPatternGenerator, CoversX86_64LoadsTheLinkerRelaxes) {
  std::vector<uint8_t> Code = sequentialCode(64);
  // call _helper
  Code[4] = 0xE8;
  MachOObjectBuilder Builder(MachO::CPU_TYPE_X86_64, Code);
  Builder.addSymbol("_caller", 0);
  Builder.addUndefined("_helper");
  Builder.addUndefined("_value");
  Builder.addRelocation(5, MachO::X86_64_RELOC_BRANCH, 2, "_helper");
  // movq _value@GOTPCREL(%rip), %rax: ld64 may rewrite the opcode two bytes
  // ahead of the field.
  Builder.addRelocation(16, MachO::X86_64_RELOC_GOT_LOAD, 2, "_value");
  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0])
                  .starts_with("40414243E8........494A4B4C4D............"
                               "5455"));
  // The CRC span reaches the function's end, so the line ends with its
  // reference.
  EXPECT_TRUE(StringRef(Out.Lines[0]).ends_with(" ^0005 _helper"));
}

TEST(MachOPatternGenerator, CoversEachInstructionAHintNames) {
  MachOObjectBuilder Builder(MachO::CPU_TYPE_ARM64, sequentialCode(64));
  // Past the start of the address space, so that a hint's address is not its
  // offset in the section.
  Builder.setTextAddress(0x100);
  Builder.addSymbol("_loader", 0);
  Builder.addUndefined("_value");
  // adrp x8, _value@GOTPAGE; ldr x8, [x8, _value@GOTPAGEOFF]; ldr x0, [x8]:
  // the linker may fold the GOT load into the last load, which carries no
  // relocation.
  Builder.addRelocation(8, MachO::ARM64_RELOC_GOT_LOAD_PAGE21, 2, "_value");
  Builder.addRelocation(12, MachO::ARM64_RELOC_GOT_LOAD_PAGEOFF12, 2, "_value");
  Builder.addHint(MCLOH_AdrpLdrGotLdr, {8, 12, 16});
  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0])
                  .starts_with("4041424344454647........................"
                               "5455565758595A5B5C5D5E5F"));
  EXPECT_EQ(Out.Stats.UnsupportedHint, 0u);
}

TEST(MachOPatternGenerator, LeavesOutAFunctionWithAnUnknownHint) {
  MachOObjectBuilder Builder(MachO::CPU_TYPE_ARM64, sequentialCode(64));
  Builder.addSymbol("_kept", 0);
  Builder.addSymbol("_hinted", 32);
  Builder.addHint(MCLOH_AdrpLdrGot + 1, {36, 40});
  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 1u);
  EXPECT_TRUE(StringRef(Out.Lines[0]).contains(":0000 _kept"));
  EXPECT_EQ(Out.Stats.UnsupportedHint, 1u);
  EXPECT_EQ(Out.Stats.UnsupportedMachOHints.count(
                {MachO::CPU_TYPE_ARM64, MCLOH_AdrpLdrGot + 1}),
            1u);
}

TEST(MachOPatternGenerator, LeavesOutEveryFunctionOfUnreadableHints) {
  // A ULEB128 that runs past the data, and an ADRP/ADD hint naming three
  // instructions.
  for (const std::vector<uint8_t> &Data :
       {std::vector<uint8_t>{MCLOH_AdrpLdrGotLdr, 3, 0x88},
        std::vector<uint8_t>{MCLOH_AdrpAdd, 3, 0, 4, 8, 0, 0, 0}}) {
    MachOObjectBuilder Builder(MachO::CPU_TYPE_ARM64, sequentialCode(64));
    Builder.addSymbol("_first", 0);
    Builder.addSymbol("_second", 32);
    Builder.setHintData(Data);
    Generated Out = generate(Builder.build());
    EXPECT_TRUE(Out.Lines.empty());
    EXPECT_EQ(Out.Stats.UnreadableHints, 1u);
    EXPECT_EQ(Out.Stats.UnsupportedHint, 2u);
  }
}

TEST(MachOPatternGenerator, HintWidthTable) {
  EXPECT_EQ(machOOptimizationHintWidth(MachO::CPU_TYPE_ARM64, MCLOH_AdrpAdd),
            4u);
  EXPECT_EQ(
      machOOptimizationHintWidth(MachO::CPU_TYPE_ARM64, MCLOH_AdrpLdrGotStr),
      4u);
  EXPECT_FALSE(machOOptimizationHintWidth(MachO::CPU_TYPE_ARM64, 0));
  EXPECT_FALSE(
      machOOptimizationHintWidth(MachO::CPU_TYPE_ARM64, MCLOH_AdrpLdrGot + 1));
  EXPECT_FALSE(
      machOOptimizationHintWidth(MachO::CPU_TYPE_X86_64, MCLOH_AdrpAdd));
}

TEST(MachOPatternGenerator, LeavesOutlinedFragmentsUnnamed) {
  MachOObjectBuilder Builder(MachO::CPU_TYPE_ARM64, sequentialCode(96));
  Builder.addSymbol("_caller", 0);
  Builder.addSymbol("_OUTLINED_FUNCTION_0", 32);
  Builder.addSymbol("_after", 64);
  // bl _OUTLINED_FUNCTION_0 and bl _after.
  Builder.addRelocation(8, MachO::ARM64_RELOC_BRANCH26, 2,
                        "_OUTLINED_FUNCTION_0");
  Builder.addRelocation(12, MachO::ARM64_RELOC_BRANCH26, 2, "_after");
  Generated Out = generate(Builder.build());
  ASSERT_EQ(Out.Lines.size(), 2u);
  // The fragment still ends the function before it, and only the branch to
  // a routine of the source is a reference.
  EXPECT_NE(Out.Lines[0].find(" 0020 :0000 _caller ^000C _after"),
            std::string::npos);
  EXPECT_EQ(Out.Lines[0].find("OUTLINED"), std::string::npos);
  EXPECT_NE(Out.Lines[1].find(":0000 _after"), std::string::npos);
  EXPECT_EQ(Out.Stats.Synthesized, 1u);
}

TEST(MachOPatternGenerator, FootprintTable) {
  auto Is = [](uint32_t CPU, uint32_t Type, unsigned Length, unsigned Before,
               unsigned Width) {
    std::optional<RelocationFootprint> F =
        machORelocationFootprint(CPU, Type, Length);
    return F && F->Before == Before && F->Width == Width;
  };
  EXPECT_TRUE(Is(MachO::CPU_TYPE_ARM64, MachO::ARM64_RELOC_BRANCH26, 2, 0, 4));
  EXPECT_TRUE(Is(MachO::CPU_TYPE_ARM64, MachO::ARM64_RELOC_ADDEND, 2, 0, 0));
  EXPECT_TRUE(Is(MachO::CPU_TYPE_ARM64, MachO::ARM64_RELOC_UNSIGNED, 3, 0, 8));
  EXPECT_TRUE(Is(MachO::CPU_TYPE_ARM64, MachO::ARM64_RELOC_UNSIGNED, 2, 0, 4));
  EXPECT_TRUE(Is(MachO::CPU_TYPE_X86_64, MachO::X86_64_RELOC_TLV, 2, 2, 4));
  EXPECT_TRUE(
      Is(MachO::CPU_TYPE_X86_64, MachO::X86_64_RELOC_SUBTRACTOR, 3, 0, 8));
  EXPECT_FALSE(machORelocationFootprint(MachO::CPU_TYPE_ARM64, 12, 2));
  EXPECT_FALSE(machORelocationFootprint(MachO::CPU_TYPE_I386,
                                        MachO::GENERIC_RELOC_VANILLA, 2));
}

TEST(MachOPatternGenerator, CollisionEvidenceRetainsSelfReferenceAliases) {
  for (bool ARM64 : {false, true}) {
    std::vector<uint8_t> Code = sequentialCode(48);
    const unsigned Field = ARM64 ? 0 : 1;
    if (ARM64) {
      Code[0] = Code[1] = Code[2] = 0;
      Code[3] = 0x94;
    } else {
      Code[0] = 0xe8;
    }
    MachOObjectBuilder Builder(
        ARM64 ? MachO::CPU_TYPE_ARM64 : MachO::CPU_TYPE_X86_64, Code);
    Builder.addSymbol("_self", 0);
    Builder.addSymbol("_self_alias", 0);
    Builder.addRelocation(Field,
                          ARM64 ? uint32_t(MachO::ARM64_RELOC_BRANCH26)
                                : uint32_t(MachO::X86_64_RELOC_BRANCH),
                          2, "_self_alias");
    const auto Ordinary = generate(Builder.build());
    ASSERT_EQ(Ordinary.Lines.size(), 1u);
    EXPECT_EQ(Ordinary.Lines.front().find('^'), std::string::npos);
    const auto Evidence = generate(Builder.build(), true);
    ASSERT_EQ(Evidence.Lines.size(), 1u);
    EXPECT_NE(Evidence.Lines.front().find(" !evidence "), std::string::npos);
    EXPECT_NE((Evidence.Lines.front() + " ")
                  .find("^000" + std::to_string(Field) + " _self_alias "),
              std::string::npos);
  }
}

TEST(MachOPatternGenerator, KeepsTheObjectsOfItsMachine) {
  const std::vector<uint8_t> ARM64 =
      MachOObjectBuilder(MachO::CPU_TYPE_ARM64, sequentialCode(32)).build();
  const std::vector<uint8_t> X86_64 =
      MachOObjectBuilder(MachO::CPU_TYPE_X86_64, sequentialCode(32)).build();
  std::unique_ptr<object::ObjectFile> A = open(ARM64), X = open(X86_64);
  ASSERT_TRUE(A && X);
  EXPECT_TRUE(isObjectForMachine(*A, TargetMachine::ARM64));
  EXPECT_FALSE(isObjectForMachine(*A, TargetMachine::X64));
  EXPECT_TRUE(isObjectForMachine(*X, TargetMachine::X64));
  EXPECT_FALSE(isObjectForMachine(*X, TargetMachine::ARM64));
}

} // namespace
