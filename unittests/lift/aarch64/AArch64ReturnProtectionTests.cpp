//===- AArch64ReturnProtectionTests.cpp - Source return protection ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/raw_ostream.h"

#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace neverd;

#define A64_RETURN_ABI_TEXT(Name, Text) constexpr char Name[] = Text;
#define A64_RETURN_ABI_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "AArch64ReturnABIFixtures.def"

MedFunc decodedFunction(std::initializer_list<uint32_t> Words,
                        BinaryFormat Format) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = Format;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = Words.size() * 4;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  size_t At = 0;
  for (uint32_t Word : Words) {
    writeLE<uint32_t>(Text.Data.data() + At, Word);
    At += 4;
  }
  Image.Segments.push_back(std::move(Text));
  Segment Callee;
  Callee.VA = 0x1100;
  Callee.Size = Callee.FileSz = 4;
  Callee.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Callee.Data.resize(4);
  writeLE<uint32_t>(Callee.Data.data(), 0xd65f03c0);
  Image.Segments.push_back(std::move(Callee));
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Image.Arch));
  Dec.setStrict(true);
  CFGBuilder Builder;
  const std::set<va_t> Entries{0x1000, 0x1100};
  Builder.setKnownFuncEntries(&Entries);
  auto Low = Builder.build(Image, Dec, 0x1000, "protected_return");
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Med = Converter.convert(Low, Image.Arch, Format);
  markSourceStackAccesses(Med, Image.Arch, Format);
  return Med;
}

std::string source(const MedFunc &Med, BinaryFormat Format,
                   Arch OutputArchitecture = Arch::AArch64) {
  auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  CEmitterOptions Options;
  Options.TheArch = OutputArchitecture;
  Options.Format = Format;
  std::string TextC;
  llvm::raw_string_ostream Out(TextC);
  EXPECT_TRUE(HighCEmitter().emit({High}, Out, Options));
  return TextC;
}

std::string source(std::initializer_list<uint32_t> Words, BinaryFormat Format,
                   Arch OutputArchitecture = Arch::AArch64) {
  return source(decodedFunction(Words, Format), Format, OutputArchitecture);
}

TEST(AArch64ReturnProtection, UnobservedLinkPhisDoNotKeepMachineProtection) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // A call on only one arm creates an LR PHI, overwritten by the reload.
    // paciasp; stp fp,lr,[sp,#-16]!; cbz x0,join; bl 0x1100;
    // join: mov x0,#7; ldp fp,lr,[sp],#16; autiasp; ret.
    const auto Text = source({0xd503233f, 0xa9bf7bfd, 0xb4000040, 0x9400003d,
                              0xd28000e0, 0xa8c17bfd, 0xd50323bf, 0xd65f03c0},
                             Format);
    EXPECT_NE(Text.find("branch-protection=pac-ret"), std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
    EXPECT_NE(Text.find("return 7;"), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, ConditionalPrologueAuthenticatesOnlyItsOwnPath) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // cbz x0,ret; paciasp; stp fp,lr,[sp,#-16]!; bl 0x1100;
    // ldp fp,lr,[sp],#16; autiasp; ret.
    const auto Text = source({0xb40000c0, 0xd503233f, 0xa9bf7bfd, 0x9400003d,
                              0xa8c17bfd, 0xd50323bf, 0xd65f03c0},
                             Format);
    EXPECT_NE(Text.find("branch-protection=pac-ret"), std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, AuthenticatedDirectAndIndirectTailTransfers) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // paciasp; stp fp,lr,[sp,#-16]!; ldp fp,lr,[sp],#16;
    // autiasp; b 0x1100 / br x16.
    for (uint32_t Branch : {0x1400003cu, 0xd61f0200u}) {
      for (bool BKey : {false, true}) {
        const auto Med = decodedFunction(
            {BKey ? 0xd503237fu : 0xd503233fu, 0xa9bf7bfd, 0xa8c17bfd,
             BKey ? 0xd50323ffu : 0xd50323bfu, Branch},
            Format);
        unsigned Tails = 0;
        for (const auto &Block : Med.Blocks)
          for (const auto &Op : Block.Ops)
            Tails += Op.IsTailCall;
        EXPECT_EQ(Tails, 2u); // The classified call and its synthetic return.
        const auto Text = source(Med, Format);
        EXPECT_NE(Text.find(BKey ? "branch-protection=pac-ret+leaf+b-key"
                                 : "branch-protection=pac-ret+leaf\""),
                  std::string::npos)
            << Text;
        EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
      }
    }
  }
}

TEST(AArch64ReturnProtection, ProvenNoReturnRetainsTheSigningPolicy) {
  for (uint32_t Transfer : {0x9400003eu, 0x1400003eu}) {
    auto Med =
        decodedFunction({0xd503233f, 0xa9bf7bfd, Transfer}, BinaryFormat::ELF);
    // The runtime declaration owns this marker; neither a missing RET nor a
    // tail classification proves no-return. Once declared, BL and B can both
    // terminate without an epilogue: the callee cannot return through LR.
    EXPECT_FALSE(findAArch64ReturnProtection(Med, BinaryFormat::ELF));
    unsigned Calls = 0;
    for (auto &Block : Med.Blocks)
      for (auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL) {
          Op.DoesNotReturn = true;
          ++Calls;
        }
    ASSERT_EQ(Calls, 1u);
    const auto Text = source(Med, BinaryFormat::ELF);
    EXPECT_NE(Text.find("branch-protection=pac-ret"), std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, UsedLinkPhiAndRecoveredArgumentStayExplicit) {
  const auto Original =
      decodedFunction({0xd503233f, 0xa9bf7bfd, 0xb4000040, 0x9400003d,
                       0xd28000e0, 0xa8c17bfd, 0xd50323bf, 0xd65f03c0},
                      BinaryFormat::ELF);
  ASSERT_TRUE(findAArch64ReturnProtection(Original, BinaryFormat::ELF));
  auto Observable = Original;
  bool Replaced = false;
  for (auto &Block : Observable.Blocks)
    for (const auto &Phi : Block.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == a64reg::X30)
        for (auto &Op : Block.Ops)
          if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == a64reg::X0 &&
              Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
              Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == 7) {
            Op.Inputs[0] = Phi.Output;
            Replaced = true;
          }
  ASSERT_TRUE(Replaced);
  EXPECT_FALSE(findAArch64ReturnProtection(Observable, BinaryFormat::ELF));

  auto Passed = Original;
  for (const auto &Block : Passed.Blocks)
    for (const auto &Phi : Block.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == a64reg::X30) {
        MedCallInfo Call;
        Call.Args.push_back(Phi.Output);
        Passed.CallInfos.push_back(Call);
      }
  ASSERT_FALSE(Passed.CallInfos.empty());
  EXPECT_FALSE(findAArch64ReturnProtection(Passed, BinaryFormat::ELF));
}

TEST(AArch64ReturnProtection, UnsafeExitsAndModifiedSaveSlotsStayExplicit) {
  for (auto Words :
       {// Conditional prologue, but its signed arm never authenticates.
        std::initializer_list<uint32_t>{0xb40000c0, 0xd503233f, 0xa9bf7bfd,
                                        0x9400003d, 0xa8c17bfd, 0xd503201f,
                                        0xd65f03c0},
        // A four-byte store overlaps the saved LR.
        {0xd503233f, 0xa9bf7bfd, 0xb9000be0, 0xa8c17bfd, 0xd50323bf,
         0xd65f03c0},
        // One predecessor reloads an uninitialized LR slot.
        {0xd503233f, 0xd10043ff, 0xb4000040, 0xf90007fe, 0xf94007fe, 0x910043ff,
         0xd50323bf, 0xd65f03c0},
        // Tail transfer with a signed LR, without AUT.
        {0xd503233f, 0xa9bf7bfd, 0xa8c17bfd, 0xd61f0200},
        // Authentication succeeds, but the outgoing SP is not restored.
        {0xd503233f, 0xa9bf7bfd, 0xa8c17bfd, 0xd50323bf, 0xd10043ff,
         0xd61f0200},
        // RET likewise needs the native entry stack position.
        {0xd503233f, 0xd50323bf, 0xd10043ff, 0xd65f03c0},
        // The authenticated address is a tail-call argument.
        {0xd503233f, 0xa9bf7bfd, 0xa8c17bfd, 0xd50323bf, 0xaa1e03e0,
         0xd61f0200},
        // An ordinary BL after AUT clobbers LR; CALL+RET is not tail proof.
        {0xd503233f, 0xd50323bf, 0x9400003e, 0xd65f03c0}}) {
    const auto Med = decodedFunction(Words, BinaryFormat::ELF);
    EXPECT_FALSE(findAArch64ReturnProtection(Med, BinaryFormat::ELF));
  }
}

TEST(AArch64ReturnProtection, VersionedEntryCopiesDoNotHideThePrologue) {
  auto Med =
      decodedFunction({0xd503233f, 0xd50323bf, 0xd65f03c0}, BinaryFormat::ELF);
  auto Copy = Med.Blocks.front().Ops.front();
  ASSERT_EQ(Copy.Opcode, NdOp::COPY);
  Copy.Output.SSAVer = 900;
  Med.Blocks.front().Ops.insert(Med.Blocks.front().Ops.begin(), Copy);
  EXPECT_TRUE(findAArch64ReturnProtection(Med, BinaryFormat::ELF));
}

TEST(AArch64ReturnProtection, LandingLabelsRetainTheEntryFrameSave) {
  auto Med = decodedFunction({0xd503233f, 0xa9bf7bfd, 0xb4000040, 0x9400003d,
                              0xd28000e0, 0xa8c17bfd, 0xd50323bf, 0xd65f03c0},
                             BinaryFormat::ELF);
  ASSERT_GT(Med.Blocks.size(), 1u);
  // Address-taken labels/landing pads are analysis roots inside this native
  // invocation, not fresh calls that bypass its entry frame initialization.
  Med.ModuleAnalysisRoots.insert(Med.Blocks.back().StartAddr);
  EXPECT_TRUE(findAArch64ReturnProtection(Med, BinaryFormat::ELF));
}

TEST(AArch64ReturnProtection, UnresolvedLocalDispatchKeepsAuthentication) {
  // A BR with an active frame may be a local switch. Missing successors do
  // not prove a tail call or a no-return function.
  const auto Text =
      source({0xd503233f, 0xa9bf7bfd, 0xd61f0200}, BinaryFormat::ELF);
  EXPECT_EQ(Text.find("branch-protection="), std::string::npos) << Text;
  EXPECT_NE(Text.find("pacia "), std::string::npos) << Text;
}

TEST(AArch64ReturnProtection, LeafKeepsKeyAndReturnValue) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // pacibsp; mov x0, #7; retab.
    const auto Text = source({0xd503237f, 0xd28000e0, 0xd65f0fff}, Format);
    EXPECT_NE(Text.find("branch-protection=pac-ret+leaf+b-key"),
              std::string::npos)
        << Text;
    EXPECT_NE(Text.find("return 7;"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("__builtin_ptrauth_auth"), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, SavedReturnAddressBecomesFunctionAttribute) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // paciasp; stp fp,lr,[sp,#-32]!; mov fp,sp; bl 0x1100;
    // mov x0,#7; ldp fp,lr,[sp],#32; autiasp; ret.
    const auto Text = source({0xd503233f, 0xa9be7bfd, 0x910003fd, 0x9400003d,
                              0xd28000e0, 0xa8c27bfd, 0xd50323bf, 0xd65f03c0},
                             Format);
    EXPECT_NE(Text.find("target(\"branch-protection=pac-ret\")"),
              std::string::npos)
        << Text;
    EXPECT_NE(Text.find("return 7;"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("stack_storage"), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, IncompleteOrObservableRecipesStayExplicit) {
  for (auto Words :
       {// PAC and AUT keys differ.
        std::initializer_list<uint32_t>{0xd503233f, 0xd50323ff, 0xd65f03c0},
        // Signed LR is the observable result: mov x0,lr.
        {0xd503233f, 0xaa1e03e0, 0xd50323bf, 0xd65f03c0},
        // No authentication.
        {0xd503233f, 0xd28000e0, 0xd65f03c0},
        // Authentication uses a different stack position.
        {0xd503233f, 0xd10043ff, 0xd50323bf, 0xd65f03c0}}) {
    const auto Text = source(Words, BinaryFormat::ELF);
    EXPECT_EQ(Text.find("branch-protection="), std::string::npos) << Text;
    EXPECT_NE(Text.find("pacia "), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, OtherTargetsCannotDiscardThePolicy) {
  EXPECT_THROW(source({0xd503237f, 0xd28000e0, 0xd65f0fff}, BinaryFormat::ELF,
                      Arch::X64),
               std::invalid_argument);
}

TEST(AArch64ReturnProtection, MalformedRecipesCannotDisappearBeforeValidation) {
  // Use the owning lowering path for register identities and entry seeds.
  // paciasp; autiasp; ret.
  const auto Function =
      decodedFunction({0xd503233f, 0xd50323bf, 0xd65f03c0}, BinaryFormat::ELF);
  ASSERT_TRUE(findAArch64ReturnProtection(Function, BinaryFormat::ELF));
  unsigned Checked = 0;
  for (size_t Index = 0; Index < Function.Blocks.front().Ops.size(); ++Index) {
    if (Function.Blocks.front().Ops[Index].Opcode != NdOp::INTRINSIC)
      continue;
    auto Broken = Function;
    Broken.Blocks.front().Ops[Index].Inputs[2].Size = 4;
    EXPECT_FALSE(findAArch64ReturnProtection(Broken, BinaryFormat::ELF));
    ++Checked;
  }
  EXPECT_EQ(Checked, 2u);
}

TEST(AArch64ReturnProtection, LLVMLeafOwnsItsSignedFrameAndUnwindPolicy) {
  for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool BKey : {false, true}) {
      const auto Function = decodedFunction(
          {BKey ? kReturnSignB : kReturnSignA, kReturnSeven,
           BKey ? kAuthenticatedReturnB : kAuthenticatedReturnA},
          Format);
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Function}, Context, kReturnModule,
                                          Arch::AArch64, {}, nullptr, Format);
      ASSERT_TRUE(Module);
      ASSERT_FALSE(llvm::verifyModule(*Module));
      const auto *Native = Module->getFunction(kProtectedFunction);
      ASSERT_NE(Native, nullptr);
      EXPECT_EQ(Native->getFnAttribute(kSigningScope).getValueAsString(),
                kSigningAll);
      EXPECT_EQ(Native->getFnAttribute(kSigningKey).getValueAsString(),
                BKey ? kSigningBKey : kSigningAKey);
      EXPECT_TRUE(Native->hasUWTable());
      EXPECT_EQ(Module->getFunction(kRawAuthenticatedReturn), nullptr);
      CEmitterOptions Options;
      Options.TheArch = Arch::AArch64;
      Options.Format = Format;
      std::string Source;
      llvm::raw_string_ostream Stream(Source);
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Stream, Options));
      EXPECT_NE(Source.find(BKey ? kSourceBKey : kSourceAKey),
                std::string::npos)
          << Source;
    }
}

TEST(AArch64ReturnProtection, LLVMNonLeafOwnsThePrivateSavedReturnAddress) {
  for (bool BKey : {false, true}) {
    const auto Function = decodedFunction(
        {BKey ? kReturnSignB : kReturnSignA, kReturnFrameSave,
         kReturnFramePointer, kReturnCall, kReturnSeven, kReturnFrameRestore,
         BKey ? kAuthenticatedReturnB : kAuthenticatedReturnA},
        BinaryFormat::ELF);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Function}, Context, kReturnModule,
                                        Arch::AArch64);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    const auto *Native = Module->getFunction(kProtectedFunction);
    ASSERT_NE(Native, nullptr);
    EXPECT_EQ(Native->getFnAttribute(kSigningScope).getValueAsString(),
              kSigningNonLeaf);
    EXPECT_EQ(Native->getFnAttribute(kSigningKey).getValueAsString(),
              BKey ? kSigningBKey : kSigningAKey);
    EXPECT_TRUE(Native->hasUWTable());
    EXPECT_EQ(Module->getFunction(kRawAuthenticatedReturn), nullptr);
  }
}

TEST(AArch64ReturnProtection, CompiledFramesDescribeTheirActualSigningPC) {
  for (auto Format : {BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool BKey : {false, true})
      for (bool Leaf : {false, true}) {
        // Darwin arm64e's unwinder owns an implicit IB return-address ABI.
        // Its CIE cannot describe an A-key override without an ABI extension.
        if (Format == BinaryFormat::MachO && !BKey)
          continue;
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(BKey);
        SCOPED_TRACE(Leaf);
        const auto Function =
            Leaf ? decodedFunction(
                       {BKey ? kReturnSignB : kReturnSignA, kReturnSeven,
                        BKey ? kAuthenticatedReturnB : kAuthenticatedReturnA},
                       Format)
                 : decodedFunction(
                       {BKey ? kReturnSignB : kReturnSignA, kReturnFrameSave,
                        kReturnFramePointer, kReturnCall, kReturnSeven,
                        kReturnFrameRestore,
                        BKey ? kAuthenticatedReturnB : kAuthenticatedReturnA},
                       Format);
        llvm::LLVMContext Context;
        auto Module = MedLLVMEmitter().emit({Function}, Context, kReturnModule,
                                            Arch::AArch64, {}, nullptr, Format);
        ASSERT_TRUE(Module);
        const auto Object = Codegen().compile(
            *Module, Arch::AArch64, Format,
            Format == BinaryFormat::MachO ? kArm64eTriple : llvm::StringRef());
        ASSERT_TRUE(Object.Success);
        auto Parsed =
            llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(
                llvm::StringRef(
                    reinterpret_cast<const char *>(Object.ObjectData.data()),
                    Object.ObjectData.size()),
                kObjectName));
        ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
        std::optional<size_t> SigningOffset, ReturnOffset;
        bool CombinedReturn = false;
        std::optional<dwarf_eh::ParseResult> Frames;
        for (const auto &Section : (*Parsed)->sections()) {
          auto Name = Section.getName();
          ASSERT_TRUE(bool(Name)) << llvm::toString(Name.takeError());
          auto Contents = Section.getContents();
          ASSERT_TRUE(bool(Contents)) << llvm::toString(Contents.takeError());
          const auto *Bytes =
              reinterpret_cast<const uint8_t *>(Contents->data());
          if (Section.isText())
            for (size_t I = 0; I + kNativeWordBytes <= Contents->size();
                 I += kNativeWordBytes) {
              const auto Word = readLE<uint32_t>(Bytes + I);
              if (Word == (BKey ? kReturnSignB : kReturnSignA)) {
                EXPECT_FALSE(SigningOffset);
                SigningOffset = I;
              }
              if (Word == kUnsignedReturn ||
                  Word ==
                      (BKey ? kAuthenticatedReturnB : kAuthenticatedReturnA)) {
                EXPECT_FALSE(ReturnOffset);
                ReturnOffset = I;
                CombinedReturn = Word != kUnsignedReturn;
              }
            }
          if (*Name == kELFFrameSection || *Name == kMachOFrameSection) {
            BinaryImage Image;
            Image.Arch = Arch::AArch64;
            Image.Bits = Bitness::Bits64;
            Image.Format = Format;
            Image.MachOCPUSubtype = kArm64eSubtype;
            Frames = dwarf_eh::parseEHFrame(
                Image, {Bytes, Contents->size(), Section.getAddress()}, {});
          }
        }
        ASSERT_TRUE(SigningOffset);
        ASSERT_TRUE(ReturnOffset);
        ASSERT_TRUE(Frames);
        ASSERT_EQ(Frames->ParseStatus, ExceptionParseStatus::Complete);
        ASSERT_EQ(Frames->CIEs.size(), 1u);
        ASSERT_EQ(Frames->FDEs.size(), 1u);
        const auto &CIE = Frames->CIEs.front();
        const auto &FDE = Frames->FDEs.front();
        EXPECT_EQ(CIE.HasPointerAuth, BKey && Format == BinaryFormat::ELF);
        EXPECT_EQ(CIE.UsesArm64eReturnAddressABI,
                  Format == BinaryFormat::MachO);
        const auto Query = [&](size_t Offset) {
          return dwarf_eh::evaluateAArch64ReturnAddressState(
              Arch::AArch64, CIE, FDE, FDE.InitialLocation + Offset);
        };
        const auto Entry = Query(*SigningOffset);
        ASSERT_TRUE(Entry.State);
        EXPECT_EQ(Entry.State->Kind, dwarf_eh::AArch64RASigningKind::Unsigned);
        const auto Signed = Query(*SigningOffset + kNativeWordBytes);
        ASSERT_TRUE(Signed.State);
        EXPECT_EQ(Signed.State->Kind, dwarf_eh::AArch64RASigningKind::SP);
        EXPECT_EQ(Signed.State->Key,
                  BKey ? dwarf_eh::AArch64RAKey::B : dwarf_eh::AArch64RAKey::A);
        const auto Return = Query(*ReturnOffset);
        ASSERT_TRUE(Return.State);
        EXPECT_EQ(Return.State->Kind,
                  CombinedReturn ? dwarf_eh::AArch64RASigningKind::SP
                                 : dwarf_eh::AArch64RASigningKind::Unsigned);
      }
}

TEST(AArch64ReturnProtection, Arm64eCannotInventAnAKeyUnwindConvention) {
  const auto Function = decodedFunction(
      {kReturnSignA, kReturnSeven, kAuthenticatedReturnA}, BinaryFormat::ELF);
  for (bool Leaf : {false, true})
    for (bool ExplicitKey : {false, true}) {
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({Function}, Context, kReturnModule,
                                          Arch::AArch64);
      ASSERT_TRUE(Module);
      auto *Native = Module->getFunction(kProtectedFunction);
      ASSERT_NE(Native, nullptr);
      Native->addFnAttr(kSigningScope, Leaf ? kSigningAll : kSigningNonLeaf);
      if (!ExplicitKey)
        Native->removeFnAttr(kSigningKey);
      testing::internal::CaptureStderr();
      const auto Object = Codegen().compile(*Module, Arch::AArch64,
                                            BinaryFormat::MachO, kArm64eTriple);
      llvm::mc_rewrite::RewriteOptions Options;
      const auto Rewrite = Codegen().compileForRewrite(
          *Module, Arch::AArch64, Options, BinaryFormat::MachO, kArm64eTriple);
      const auto Diagnostic = testing::internal::GetCapturedStderr();
      EXPECT_FALSE(Object.Success);
      EXPECT_TRUE(Object.ObjectData.empty());
      EXPECT_FALSE(Rewrite.ImageValid);
      EXPECT_NE(Diagnostic.find(kArm64eUnwindDiagnostic), std::string::npos)
          << Diagnostic;
    }
}

TEST(AArch64ReturnProtection, LLVMCRejectsLostOrMalformedSigningPolicy) {
  const auto Function = decodedFunction(
      {kReturnSignA, kReturnSeven, kAuthenticatedReturnA}, BinaryFormat::ELF);
  for (unsigned Case = 0; Case != 4; ++Case) {
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Function}, Context, kReturnModule,
                                        Arch::AArch64);
    ASSERT_TRUE(Module);
    auto *Native = Module->getFunction(kProtectedFunction);
    ASSERT_NE(Native, nullptr);
    if (Case == 1)
      Native->addFnAttr(kSigningScope, kBadSigningScope);
    if (Case == 2)
      Native->addFnAttr(kSigningKey, kBadSigningKey);
    if (Case == 3)
      Native->addFnAttr(kPCSigningAttribute);
    CEmitterOptions Options;
    Options.TheArch = Case == 0 ? Arch::X64 : Arch::AArch64;
    std::string Source;
    llvm::raw_string_ostream Stream(Source);
    EXPECT_THROW(LLVMCEmitter().emit(*Module, Stream, Options),
                 std::invalid_argument);
    EXPECT_TRUE(Source.empty()) << Source;
  }
}
} // namespace
