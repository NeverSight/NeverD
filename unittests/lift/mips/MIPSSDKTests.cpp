//===- MIPSSDKTests.cpp - Physical listings and startup arguments -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// SDK consumers must distinguish physical rows from delayed execution units.
//===----------------------------------------------------------------------===//

#include "InstructionFlow.h"
#include "MIPSFixtures.h"
#include "SessionTestAccess.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

using namespace neverd;

namespace {

class MIPSSDK : public ::testing::TestWithParam<Arch> {
protected:
  llvm::SmallString<128> Directory;
  neverd_session_t Session = nullptr;

  void SetUp() override {
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-mips", Directory));
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    std::error_code Error;
    std::filesystem::remove_all(std::filesystem::path(Directory.str().str()),
                                Error);
    EXPECT_FALSE(Error);
  }
  std::string take(const char *Text) {
    std::string Result = Text ? Text : "";
    neverd_free_string(Text);
    return Result;
  }
  BinaryImage image(llvm::ArrayRef<uint32_t> Words) {
    BinaryImage Result;
    Result.Arch = GetParam();
    Result.Bits = isMIPS64(GetParam()) ? Bitness::Bits64 : Bitness::Bits32;
    Result.Format = BinaryFormat::ELF;
    Result.Entry = Result.Base = 0x1000;
    Segment Text;
    Text.VA = 0x1000;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    for (uint32_t Word : Words)
      for (unsigned I = 0; I < 4; ++I)
        Text.Data.push_back(Word >>
                            (8 * (archLittleEndian(GetParam()) ? I : 3 - I)));
    Text.Size = Text.FileSz = Text.Data.size();
    Result.Segments.push_back(std::move(Text));
    return Result;
  }
};

TEST_P(MIPSSDK, FunctionPagesKeepTheHeadAndSlotAsSeparateRows) {
  mips_test::ELFBytes File(GetParam());
  File.put(0x104, isMIPS64(GetParam()) ? 0x67bd0010 : 0x27bd0010, 4);
  const std::string Path =
      (std::filesystem::path(Directory.str().str()) / "slot.elf").string();
  std::error_code Error;
  llvm::raw_fd_ostream Output(Path, Error);
  ASSERT_FALSE(Error);
  Output.write(reinterpret_cast<const char *>(File.Bytes.data()),
               File.Bytes.size());
  Output.close();
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << take(neverd_last_error(Session));
  for (unsigned I = 0; I < 2; ++I) {
    const std::string Text = take(
        neverd_function_disasm_json(Session, 0x10100, 0x10100 + 4 * I, 1,
                                    NEVERD_DISASM_FLOW | NEVERD_DISASM_STACK));
    auto Page = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Page)) << Text << take(neverd_last_error(Session));
    const auto *Object = Page->getAsObject();
    ASSERT_NE(Object, nullptr);
    const auto *Rows = Object->getArray("instructions");
    ASSERT_NE(Rows, nullptr);
    ASSERT_EQ(Rows->size(), 1u);
    const auto *Row = Rows->front().getAsObject();
    ASSERT_NE(Row, nullptr);
    EXPECT_EQ(Row->getInteger("size"), 4);
    EXPECT_EQ(Row->getString("addr"), I ? "0x10104" : "0x10100");
    EXPECT_EQ(Row->getInteger("sp"), I ? 16 : 0);
    if (!I) {
      EXPECT_EQ(Row->getString("mnemonic"), "jr");
      EXPECT_EQ(Object->getString("next_address"), "0x10104");
    } else {
      EXPECT_EQ(Row->getString("mnemonic"),
                isMIPS64(GetParam()) ? "daddiu" : "addiu");
      ASSERT_NE(Object->get("next_address"), nullptr);
      EXPECT_TRUE(Object->get("next_address")->getAsNull().has_value());
    }
  }
}

TEST_P(MIPSSDK, BenchmarkConsumesTheLoadedImageProfile) {
  mips_test::ELFBytes File(GetParam());
  File.abiFlags();
  if (!File.Is64) {
    File.Bytes[0xe3] = 5;
    File.Bytes[0xe5] = 3;
    File.Bytes[0xe7] = 6;
    File.put(0xec, 0x200, 4);
  }
  const size_t Program = File.Is64 ? 64 : 52;
  const size_t Width = File.Is64 ? 8 : 4;
  File.put(Program + (File.Is64 ? 8 : 4), 0x100, Width);
  File.put(Program + (File.Is64 ? 16 : 8), 0x10100, Width);
  File.put(Program + (File.Is64 ? 24 : 12), 0x10100, Width);
  File.put(Program + (File.Is64 ? 32 : 16), 16, Width);
  File.put(Program + (File.Is64 ? 40 : 20), 16, Width);
  File.put(0x100, File.Is64 ? 0x0085102d : 0x7b02201e, 4);
  File.put(0x104, File.Is64 ? 0u : 0x78b00099u, 4);
  File.put(0x108, 0x03e00008, 4);
  File.put(0x10c, 0, 4);
  const auto Path =
      (std::filesystem::path(Directory.str().str()) / "benchmark.elf").string();
  std::error_code Error;
  llvm::raw_fd_ostream Output(Path, Error);
  ASSERT_FALSE(Error);
  Output.write(reinterpret_cast<const char *>(File.Bytes.data()),
               File.Bytes.size());
  Output.close();
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << take(neverd_last_error(Session));
  const auto Text = take(neverd_bench_decode(Session));
  auto Result = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Result)) << Text << take(neverd_last_error(Session));
  const auto *Object = Result->getAsObject();
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(Object->getString("arch"), getArchName(GetParam()));
  EXPECT_EQ(Object->getInteger("exec_bytes"), 16);
  EXPECT_EQ(Object->getInteger("bench_bytes"), 16);
  EXPECT_EQ(Object->getInteger("insns"), 4);
}

TEST_P(MIPSSDK, FlowInspectionPreservesTheBorrowedPhysicalDecode) {
  auto Image = image({0x50800003, 0x27bd0010, 0x03e00008, 0, 0x03e00008, 0});
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  DecodedInsn Instruction;
  ASSERT_EQ(Decode.decodeOne(Image.Segments[0].Data.data(), 24, Image.Entry,
                             Instruction),
            4);
  const std::string Before = Instruction.Raw->mnemonic;
  const auto Flow =
      sdk::SessionTestAccess::instructionFlow(Image, Decode, Instruction);
  EXPECT_EQ(Flow.Kind, "cjump");
  EXPECT_EQ(Flow.Target, 0x1010u);
  EXPECT_EQ(Flow.DelaySlotBytes, 4);
  EXPECT_EQ(Flow.Stack.Delta, 0);
  EXPECT_EQ(std::string(Instruction.Raw->mnemonic), Before);
  EXPECT_EQ(Instruction.Raw->address, Image.Entry);
}

TEST_P(MIPSSDK, StartupMainArgumentCanBePreparedInTheCallSlot) {
  auto Image =
      image({0x0c000800, isMIPS64(GetParam()) ? 0x64041010u : 0x24041010u,
             0x03e00008, 0, 0x03e00008, 0});
  Import Startup;
  Startup.Name = "__uClibc_main";
  Startup.IATAddr = 0x2000;
  Image.Imports.push_back(std::move(Startup));
  EXPECT_EQ(sdk::SessionTestAccess::programMain(Image), 0x1010u);
}

TEST_P(MIPSSDK, FileInstallationRequiresAnArchitectureSpecificWriter) {
  mips_test::ELFBytes File(GetParam());
  const std::filesystem::path Root(Directory.str().str());
  const std::string Input = (Root / "input.elf").string();
  const std::string Output = (Root / "installed.elf").string();
  std::error_code Error;
  llvm::raw_fd_ostream Stream(Input, Error);
  ASSERT_FALSE(Error);
  Stream.write(reinterpret_cast<const char *>(File.Bytes.data()),
               File.Bytes.size());
  Stream.close();
  ASSERT_TRUE(neverd_session_load(Session, Input.c_str()));
  for (int Strategy : {0, 1}) {
    SCOPED_TRACE(Strategy);
    testing::internal::CaptureStderr();
    int Installed =
        neverd_patch_from_ir(Session, "define i32 @func_10100() { ret i32 37 }",
                             Strategy, Output.c_str());
    auto Detail = testing::internal::GetCapturedStderr();
    EXPECT_EQ(Installed, 0);
    EXPECT_NE(Detail.find("unsupported arch"), std::string::npos) << Detail;
    EXPECT_FALSE(std::filesystem::exists(Output));
  }
}

class MicroMIPSSDK : public MIPSSDK {};

class MIPSObjectSDK : public MIPSSDK {};

TEST_P(MIPSObjectSDK, LiftToObjectRetainsABIAndReportsTheEntryBody) {
  auto Target = GetParam();
  // n32 uses the ELF32 header but has the same 64-bit GPR file as n64.
  mips_test::ELFBytes File(
      isMIPSN32(Target)
          ? (archLittleEndian(Target) ? Arch::MIPS32EL : Arch::MIPS32)
          : Target);
  File.abiFlags();
  if (isMIPSN32(Target)) {
    File.put(36, llvm::ELF::EF_MIPS_ARCH_64R2 | llvm::ELF::EF_MIPS_ABI2, 4);
    File.Bytes[0xe2] = 64;
    File.Bytes[0xe4] = 2;
  }
  File.put(0x104, 0x24020025, 4); // addiu v0,zero,37 in the return slot
  const auto Path =
      (std::filesystem::path(Directory.str().str()) / "leaf.elf").string();
  std::error_code Error;
  llvm::raw_fd_ostream Output(Path, Error);
  ASSERT_FALSE(Error);
  Output.write(reinterpret_cast<const char *>(File.Bytes.data()),
               File.Bytes.size());
  Output.close();

  ASSERT_EQ(neverd_lift_to_obj(Session, Path.c_str(), 0, 1), 0)
      << take(neverd_last_error(Session));
  unsigned long long Length = 0;
  const auto *Obj = neverd_roundtrip_obj(Session, &Length);
  ASSERT_NE(Obj, nullptr);
  ASSERT_GE(Length, File.Is64 ? 64u : 52u);
  EXPECT_EQ(Obj[llvm::ELF::EI_CLASS],
            File.Is64 ? llvm::ELF::ELFCLASS64 : llvm::ELF::ELFCLASS32);
  EXPECT_EQ(Obj[llvm::ELF::EI_DATA],
            File.Little ? llvm::ELF::ELFDATA2LSB : llvm::ELF::ELFDATA2MSB);
  const auto Endian =
      File.Little ? llvm::endianness::little : llvm::endianness::big;
  EXPECT_EQ(llvm::support::endian::read16(Obj + 16, Endian), llvm::ELF::ET_REL);
  EXPECT_EQ(llvm::support::endian::read16(Obj + 18, Endian),
            llvm::ELF::EM_MIPS);
  const auto Flags =
      llvm::support::endian::read32(Obj + (File.Is64 ? 48 : 36), Endian);
  EXPECT_EQ(bool(Flags & llvm::ELF::EF_MIPS_ABI2), isMIPSN32(Target));
  ASSERT_EQ(neverd_roundtrip_func_count(Session), 1);
  EXPECT_GT(neverd_roundtrip_func_size(Session, 0), 0u);
}

INSTANTIATE_TEST_SUITE_P(ABIAndByteOrder, MIPSObjectSDK,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64N32, Arch::MIPS64N32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

TEST_P(MicroMIPSSDK, PhysicalPagesKeepTwoByteHeadAndFourByteSlot) {
  mips_test::ELFBytes File(GetParam());
  File.put(36, 0x72001000, 4);
  File.put(24, 0x10101, 4);
  File.put(0x100, 0x459f, 2); // jr16 ra
  File.put(0x102, 0x33bd, 2); // addiu sp,sp,16
  File.put(0x104, 16, 2);
  const std::string Path =
      (std::filesystem::path(Directory.str().str()) / "micro-slot.elf")
          .string();
  std::error_code Error;
  llvm::raw_fd_ostream Output(Path, Error);
  ASSERT_FALSE(Error);
  Output.write(reinterpret_cast<const char *>(File.Bytes.data()),
               File.Bytes.size());
  Output.close();
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << take(neverd_last_error(Session));
  for (unsigned I = 0; I != 2; ++I) {
    const std::string Text = take(
        neverd_function_disasm_json(Session, 0x10100, 0x10100 + 2 * I, 1,
                                    NEVERD_DISASM_FLOW | NEVERD_DISASM_STACK));
    auto Page = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Page)) << Text << take(neverd_last_error(Session));
    const auto *Object = Page->getAsObject();
    ASSERT_NE(Object, nullptr);
    const auto *Rows = Object->getArray("instructions");
    ASSERT_NE(Rows, nullptr);
    ASSERT_EQ(Rows->size(), 1u);
    const auto *Row = Rows->front().getAsObject();
    ASSERT_NE(Row, nullptr);
    EXPECT_EQ(Row->getInteger("size"), I ? 4 : 2);
    EXPECT_EQ(Row->getString("addr"), I ? "0x10102" : "0x10100");
    EXPECT_EQ(Row->getInteger("sp"), I ? 16 : 0);
    if (!I)
      EXPECT_EQ(Object->getString("next_address"), "0x10102");
  }
}

TEST_P(MicroMIPSSDK, CachedFlowDecoderSwitchesModesWithoutReplacingTheListing) {
  for (bool Micro : {false, true, false, true}) {
    auto Image = image({0x0080f809, 0}); // classic jalr a0; nop
    Image.Mode = InstructionMode::MixedMIPS;
    Image.MIPS.emplace();
    Image.MIPS->HasMicroMIPS = true;
    Image.MIPS->CodeModeEntries[Image.Entry] =
        Micro ? InstructionMode::MicroMIPS : InstructionMode::MIPSClassic;
    if (Micro) {
      auto &Text = Image.Segments.front();
      Text.Data.clear();
      for (uint16_t Half : {0x45e4, 0x0c9f}) { // jalrs16 a0; move a0,ra
        Text.Data.push_back(Half >> (archLittleEndian(GetParam()) ? 0 : 8));
        Text.Data.push_back(Half >> (archLittleEndian(GetParam()) ? 8 : 0));
      }
      Text.Size = Text.FileSz = Text.Data.size();
    }
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Image));
    const auto &Code = Image.Segments.front().Data;
    DecodedInsn Instruction;
    ASSERT_EQ(
        Decode.decodeOne(Code.data(), Code.size(), Image.Entry, Instruction),
        Micro ? 2 : 4);
    const std::string Before = Instruction.Raw->mnemonic;
    const auto Flow =
        sdk::SessionTestAccess::instructionFlow(Image, Decode, Instruction);
    EXPECT_EQ(Flow.Kind, "icall");
    EXPECT_EQ(Flow.DelaySlotBytes, Micro ? 2 : 4);
    EXPECT_EQ(Instruction.Raw->address, Image.Entry);
    EXPECT_EQ(Instruction.Raw->size, Micro ? 2 : 4);
    EXPECT_EQ(std::string(Instruction.Raw->mnemonic), Before);
  }
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MicroMIPSSDK,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

class MIPS16SDK : public MIPSSDK {};

TEST_P(MIPS16SDK, CallTreeFollowsProvenClassicEntryTransportWithoutTypes) {
  for (unsigned Change = 0; Change < 4; ++Change) {
    SCOPED_TRACE(Change);
    mips_test::ELFBytes File(GetParam());
    File.Bytes.resize(0x3a0);
    File.put(52 + 16, File.Bytes.size(), 4);
    File.put(52 + 20, File.Bytes.size(), 4);
    File.abiFlags();
    File.Bytes[0xe5] = 1;
    File.Bytes[0xe7] = 1;
    File.put(36, 0x74001000, 4);
    File.put(0xec, 0x400, 4);
    // GNU's linked classic caller, compressed body and local entry stub.
    const uint32_t Caller[] = {0x44856000, 0x27bdffe8, 0xafbf0014, 0x0c004058,
                               0x00802825, 0x8fbf0014, 0x03e00008, 0x27bd0018};
    const uint16_t Callee[] = {0x6745, 0xe820, 0xea8e};
    const uint32_t Adapter[] = {0x3c190001, 0x27390141, 0x03200008, 0x44046000};
    for (unsigned I = 0; I < std::size(Caller); ++I)
      File.put(0x100 + I * 4, Caller[I], 4);
    for (unsigned I = 0; I < std::size(Callee); ++I)
      File.put(0x140 + I * 2, Callee[I], 2);
    for (unsigned I = 0; I < std::size(Adapter); ++I)
      File.put(0x160 + I * 4, Adapter[I], 4);
    if (Change == 1)
      File.put(0x164, 0x27390140, 4); // missing destination mode tag
    if (Change == 2)
      File.put(0x164, 0x03e0c825, 4); // t9 depends on the live return link
    if (Change == 3)
      File.put(0x16c, 0xafbf0000, 4); // store in the actual delay slot
    // Real ELF symbols provide the extents and mode entries. There is no
    // declaration or debug context for the graph to borrow a call contract.
    File.put(32, 0x300, 4);
    File.put(48, 4, 2);
    const char Names[] = "\0entry_adapter\0fp_callee\0kernel\0";
    std::copy(std::begin(Names), std::end(Names), File.Bytes.begin() + 0x250);
    unsigned Index = 1;
    for (const auto &[Name, Address, Size, Info, Mode] :
         {std::tuple{1u, 0x10160u, 16u, 2u, 0u},
          std::tuple{15u, 0x10140u, 6u, 18u, 0xf0u},
          std::tuple{25u, 0x10100u, 32u, 18u, 0u}}) {
      const size_t S = 0x200 + 16 * Index++;
      File.put(S, Name, 4);
      File.put(S + 4, Address, 4);
      File.put(S + 8, Size, 4);
      File.Bytes[S + 12] = Info;
      File.Bytes[S + 13] = Mode;
      File.put(S + 14, 1, 2);
    }
    File.put(0x328 + 4, 1, 4); // text: SHT_PROGBITS, SHF_ALLOC|EXEC
    File.put(0x328 + 8, 6, 4);
    File.put(0x328 + 12, 0x10100, 4);
    File.put(0x328 + 16, 0x100, 4);
    File.put(0x328 + 20, 0x70, 4);
    File.put(0x328 + 32, 4, 4);
    File.put(0x350 + 4, 2, 4); // symtab -> strtab
    File.put(0x350 + 16, 0x200, 4);
    File.put(0x350 + 20, 64, 4);
    File.put(0x350 + 24, 3, 4);
    File.put(0x350 + 28, 2, 4);
    File.put(0x350 + 32, 4, 4);
    File.put(0x350 + 36, 16, 4);
    File.put(0x378 + 4, 3, 4); // strtab
    File.put(0x378 + 16, 0x250, 4);
    File.put(0x378 + 20, sizeof(Names), 4);
    File.put(0x378 + 32, 1, 4);
    const auto Path =
        (std::filesystem::path(Directory.str().str()) / "entry-stub.elf")
            .string();
    std::error_code Error;
    llvm::raw_fd_ostream Output(Path, Error);
    ASSERT_FALSE(Error);
    Output.write(reinterpret_cast<const char *>(File.Bytes.data()),
                 File.Bytes.size());
    Output.close();
    ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
        << take(neverd_last_error(Session));
    const auto Text = take(neverd_call_tree_json(
        Session, "{\"root\":\"0x10100\",\"max_depth\":3}"));
    auto Tree = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Tree)) << Text << take(neverd_last_error(Session));
    const auto *Root = Tree->getAsObject();
    ASSERT_NE(Root, nullptr);
    EXPECT_EQ(Root->getBoolean("complete"), true);
    EXPECT_EQ(Root->getBoolean("depth_limited"), false);
    const auto *Order = Root->getArray("bottom_up");
    ASSERT_NE(Order, nullptr);
    std::vector<std::string> Entries;
    for (const auto &Value : *Order)
      Entries.push_back(Value.getAsString().value_or("").str());
    EXPECT_EQ(Entries, Change ? (std::vector<std::string>{"0x10160", "0x10100"})
                              : (std::vector<std::string>{"0x10140", "0x10160",
                                                          "0x10100"}))
        << Text;
  }
}

TEST_P(MIPS16SDK, PhysicalPagesKeepExtendPairsAndTwoByteDelaySlots) {
  mips_test::ELFBytes File(GetParam());
  File.put(36, 0x74001000, 4);
  File.put(24, 0x10101, 4);
  File.put(0x100, 0xf010, 2); // li v0,0x8000, one four-byte instruction
  File.put(0x102, 0x6a00, 2);
  File.put(0x104, 0xe820, 2); // jr ra
  File.put(0x106, 0x6301, 2); // addiu sp,8, one two-byte slot
  const auto Path =
      (std::filesystem::path(Directory.str().str()) / "mips16-slot.elf")
          .string();
  std::error_code Error;
  llvm::raw_fd_ostream Output(Path, Error);
  ASSERT_FALSE(Error);
  Output.write(reinterpret_cast<const char *>(File.Bytes.data()),
               File.Bytes.size());
  Output.close();
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << take(neverd_last_error(Session));
  for (unsigned Offset : {0u, 4u, 6u}) {
    const auto Text = take(
        neverd_function_disasm_json(Session, 0x10100, 0x10100 + Offset, 1,
                                    NEVERD_DISASM_FLOW | NEVERD_DISASM_STACK));
    auto Page = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Page)) << Text << take(neverd_last_error(Session));
    const auto *Object = Page->getAsObject();
    ASSERT_NE(Object, nullptr);
    const auto *Rows = Object->getArray("instructions");
    ASSERT_NE(Rows, nullptr);
    ASSERT_EQ(Rows->size(), 1u);
    const auto *Row = Rows->front().getAsObject();
    ASSERT_NE(Row, nullptr);
    EXPECT_EQ(Row->getInteger("size"), Offset ? 2 : 4);
    EXPECT_EQ(Row->getInteger("sp"), Offset == 6 ? 8 : 0);
    EXPECT_EQ(Row->getString("addr"), Offset == 0   ? "0x10100"
                                      : Offset == 4 ? "0x10104"
                                                    : "0x10106");
    if (Offset != 6)
      EXPECT_EQ(Object->getString("next_address"),
                Offset ? "0x10106" : "0x10104");
    else {
      ASSERT_NE(Object->get("next_address"), nullptr);
      EXPECT_TRUE(Object->get("next_address")->getAsNull().has_value());
    }
  }
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPS16SDK,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

INSTANTIATE_TEST_SUITE_P(Profiles, MIPSSDK,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

} // namespace
