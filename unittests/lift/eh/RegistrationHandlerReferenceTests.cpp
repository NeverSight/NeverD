//===- RegistrationHandlerReferenceTests.cpp - Retained handler identities===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// SafeSEH proves source entry identity without proving EH behavior.
//===----------------------------------------------------------------------===//

#include "../core/MappedSectionFlags.h"
#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/Verifier.h"

namespace {
using namespace neverd;

struct RetainedHandlerImage {
  static constexpr va_t Entry = 0x401000;
  static constexpr va_t Handler = Entry + 0xc0;
  static constexpr va_t Config = 0x402000;
  static constexpr va_t Table = Config + 0x80;
  BinaryImage Image;
  RetainedHandlerImage() {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = Entry;
    Image.DynInfo.LoadConfigRVA = Config - Image.Base;
    Image.DynInfo.LoadConfigSize = 0x48;
    Segment Text;
    Text.Name = ".text";
    Text.VA = Entry;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100, 0xcc);
    Text.Data[0] = Text.Data[0xc0] = 0xc3;
    Text.Size = Text.FileSz = Text.Data.size();
    Segment Data;
    Data.Name = ".rdata";
    Data.VA = Config;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.resize(0x100);
    writeLE<uint32_t>(Data.Data.data(), 0x48);
    writeLE<uint32_t>(Data.Data.data() + 0x40, Table);
    writeLE<uint32_t>(Data.Data.data() + 0x44, 1);
    writeLE<uint32_t>(Data.Data.data() + 0x80, Handler - Image.Base);
    Data.Size = Data.FileSz = Data.Data.size();
    Image.Segments = {Text, Data};
    for (const auto &Segment : Image.Segments) {
      Section Section;
      Section.Name = Segment.Name;
      Section.VA = Segment.VA;
      Section.Size = Section.FileSz = Segment.Size;
      Section.Flags = Segment.Flags;
      Image.Sections.push_back(Section);
    }
    Image.BaseRelocations.push_back(
        {Config + 0x40, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
    Image.DataPtrRelocSlots.insert(Config + 0x40);
    Image.DataPtrRelocTargetOwners[Config + 0x40] = Config;
  }

  MedFunc sourceReference(va_t Address) const {
    MedFunc Function;
    Function.Entry = Entry;
    Function.Name = "handler_reference";
    Function.ReturnType = NdType::makeInt(4);
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Entry;
    Block.EndAddr = Entry + 1;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Entry;
    Return.addInput(
        MedVar::makeConst(Address, 4, ConstantAddressProvenance::CodeAddress));
    Block.Ops.push_back(Return);
    Function.Blocks.push_back(std::move(Block));
    ExceptionFunction EH;
    EH.CodeRange = {Entry, Entry + 0x20};
    EH.ParseStatus = ExceptionParseStatus::Partial;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
    EH.Personality = ExceptionPersonality::Unknown;
    EH.PersonalityVA = Handler;
    EH.Registration.emplace();
    EH.Registration->HandlerVA = Handler;
    EH.Registration->Scopes.push_back({-1, 0, Entry + 0x10, true});
    Function.ExceptionMetadata = std::move(EH);
    return Function;
  }
};

TEST(SafeSEHReference, RequiresImmutableCompleteImageEvidence) {
  RetainedHandlerImage Original;
  ASSERT_TRUE(coff_loader::isCheckedX86SafeSEHHandlerReference(
      Original.Image, Original.Handler));
  EXPECT_FALSE(coff_loader::getCheckedX86EH4CookieCheck(Original.Image,
                                                        Original.Handler));
  EXPECT_FALSE(coff_loader::isCheckedX86CxxHandlerReference(Original.Image,
                                                            Original.Handler));
  for (unsigned Mutation = 0; Mutation != 30; ++Mutation) {
    SCOPED_TRACE(Mutation);
    RetainedHandlerImage F;
    auto &Image = F.Image;
    switch (Mutation) {
    case 0:
      Image.Arch = Arch::ARM;
      break;
    case 1:
      Image.Bits = Bitness::Bits64;
      break;
    case 2:
      Image.Format = BinaryFormat::ELF;
      break;
    case 3:
      Image.IsRelocatable = true;
      break;
    case 4:
      Image.DynInfo.LoadConfigSize = 0x44;
      break;
    case 5:
      writeLE<uint32_t>(Image.Segments[1].Data.data() + 0x44, 2);
      break;
    case 6:
      writeLE<uint32_t>(Image.Segments[1].Data.data() + 0x44, 4097);
      break;
    case 7:
      writeLE<uint32_t>(Image.Segments[1].Data.data() + 0x80, 0x20c0);
      break;
    case 8:
      Image.Segments[1].Flags =
          Image.Segments[1].Flags | SegmentFlags::Writable;
      break;
    case 9:
      setMappedSectionFlags(Image, Image.Sections[1],
                            Image.Sections[1].Flags | SegmentFlags::Writable);
      break;
    case 10:
      Image.Segments.push_back(Image.Segments[1]);
      break;
    case 11:
      Image.Sections.push_back(Image.Sections[1]);
      break;
    case 12:
      Image.Segments[1].FileSz = 0x82;
      break;
    case 13:
      Image.Sections[1].FileSz = 0x82;
      break;
    case 14:
      Image.BaseRelocations.clear();
      break;
    case 15:
      Image.BaseRelocations.push_back(Image.BaseRelocations.front());
      break;
    case 16:
      Image.BaseRelocations.front().Type = llvm::COFF::IMAGE_REL_BASED_DIR64;
      break;
    case 17:
      Image.DataPtrRelocTargetOwners[F.Config + 0x40] = F.Config + 4;
      break;
    case 18:
      Image.Segments[0].Flags = SegmentFlags::Readable;
      break;
    case 19:
      Image.Sections[0].Flags = SegmentFlags::Readable;
      break;
    case 20:
      Image.Segments[0].Flags =
          Image.Segments[0].Flags | SegmentFlags::Writable;
      break;
    case 21:
      Image.BaseRelocations.push_back(
          {F.Table, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      break;
    case 22:
      Image.BaseRelocations.push_back(
          {F.Handler, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      break;
    case 23:
      writeLE<uint32_t>(Image.Segments[1].Data.data(), 0x44);
      break;
    case 24:
      writeLE<uint32_t>(Image.Segments[1].Data.data(), 0x4c);
      break;
    case 25:
      writeLE<uint32_t>(Image.Segments[1].Data.data() + 0x44, 2);
      writeLE<uint32_t>(Image.Segments[1].Data.data() + 0x84,
                        F.Handler - Image.Base);
      break;
    case 26:
      writeLE<uint32_t>(Image.Segments[1].Data.data() + 0x44, 2);
      writeLE<uint32_t>(Image.Segments[1].Data.data() + 0x84,
                        F.Handler - Image.Base - 1);
      break;
    case 27:
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      break;
    case 28:
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      break;
    case 29:
      Image.Format = BinaryFormat::MachO;
      break;
    }
    EXPECT_FALSE(
        coff_loader::isCheckedX86SafeSEHHandlerReference(Image, F.Handler));
  }
  EXPECT_FALSE(coff_loader::isCheckedX86SafeSEHHandlerReference(
      Original.Image, Original.Handler + 1));
}

TEST(SafeSEHReference, EverySortedHandlerHasItsOwnIdentity) {
  RetainedHandlerImage F;
  writeLE<uint32_t>(F.Image.Segments[1].Data.data() + 0x44, 2);
  writeLE<uint32_t>(F.Image.Segments[1].Data.data() + 0x80,
                    F.Handler - F.Image.Base - 16);
  writeLE<uint32_t>(F.Image.Segments[1].Data.data() + 0x84,
                    F.Handler - F.Image.Base);
  EXPECT_TRUE(coff_loader::isCheckedX86SafeSEHHandlerReference(F.Image,
                                                               F.Handler - 16));
  EXPECT_TRUE(
      coff_loader::isCheckedX86SafeSEHHandlerReference(F.Image, F.Handler));
  EXPECT_FALSE(
      coff_loader::isCheckedX86SafeSEHHandlerReference(F.Image, F.Handler - 8));
}

TEST(SafeSEHReference, SourceDeclarationDoesNotGrantNativeExceptionSemantics) {
  for (bool KnownPersonality : {false, true}) {
    for (bool InvalidTable : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << KnownPersonality << ':' << InvalidTable);
      RetainedHandlerImage F;
      if (InvalidTable)
        writeLE<uint32_t>(F.Image.Segments[1].Data.data() + 0x80,
                          F.Handler - F.Image.Base + 1);
      auto Function = F.sourceReference(F.Handler);
      if (KnownPersonality)
        Function.ExceptionMetadata->Personality =
            ExceptionPersonality::ExceptHandler4;
      llvm::LLVMContext Context;
      MedLLVMEmitter Emitter;
      auto Module = Emitter.emit({Function}, Context, "handler-reference",
                                 Arch::X86, {}, &F.Image, BinaryFormat::COFF);
      if (InvalidTable) {
        EXPECT_EQ(Module, nullptr);
        continue;
      }
      ASSERT_NE(Module, nullptr);
      EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      auto *Handler = Module->getFunction("__nd_registration_handler_4010C0");
      ASSERT_NE(Handler, nullptr);
      EXPECT_TRUE(Handler->isDeclaration());
      auto Identity = rewrite_source::getOriginalVA(*Handler);
      ASSERT_TRUE(bool(Identity));
      ASSERT_TRUE(Identity->has_value());
      EXPECT_EQ(**Identity, F.Handler);
      auto Contract =
          exception_rewrite::validateExceptionRewriteContracts(*Module);
      EXPECT_FALSE(bool(Contract));
      if (!Contract)
        llvm::consumeError(Contract.takeError());
    }
  }
}
TEST(SafeSEHReference, HandlerIdentityDoesNotDependOnBodyOrderOrSharding) {
  for (bool Reverse : {false, true})
    for (bool MaskParent : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Reverse << ':' << MaskParent);
      RetainedHandlerImage F;
      auto Parent = F.sourceReference(F.Handler);
      auto Ordinary = F.sourceReference(F.Handler);
      Ordinary.Entry += 0x40;
      Ordinary.Name = "ordinary_handler_reference";
      Ordinary.ExceptionMetadata.reset();
      Ordinary.Blocks[0].StartAddr = Ordinary.Entry;
      Ordinary.Blocks[0].EndAddr = Ordinary.Entry + 1;
      Ordinary.Blocks[0].Ops[0].Addr = Ordinary.Entry;
      std::vector<MedFunc> Functions{Parent, Ordinary};
      std::vector<char> Mask{MaskParent ? char(0) : char(1), 1};
      if (Reverse) {
        std::reverse(Functions.begin(), Functions.end());
        std::reverse(Mask.begin(), Mask.end());
      }
      llvm::LLVMContext Context;
      MedLLVMEmitter Emitter;
      auto Module = Emitter.emit(Functions, Context, "handler-order", Arch::X86,
                                 {}, &F.Image, BinaryFormat::COFF, true, &Mask);
      ASSERT_NE(Module, nullptr);
      EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      auto *Handler = Module->getFunction("__nd_registration_handler_4010C0");
      ASSERT_NE(Handler, nullptr);
      EXPECT_TRUE(Handler->isDeclaration());
      EXPECT_FALSE(Handler->use_empty());
      EXPECT_EQ(Module->getFunction(Parent.Name)->isDeclaration(), MaskParent);
      EXPECT_FALSE(Module->getFunction(Ordinary.Name)->isDeclaration());
    }
}

TEST(SafeSEHReference, StandaloneConsumerNeedsNoParentMetadata) {
  for (bool ValidTable : {false, true}) {
    SCOPED_TRACE(ValidTable);
    RetainedHandlerImage F;
    auto Consumer = F.sourceReference(F.Handler);
    Consumer.ExceptionMetadata.reset();
    if (!ValidTable)
      F.Image.BaseRelocations.clear();
    llvm::LLVMContext Context;
    MedLLVMEmitter Emitter;
    auto Module = Emitter.emit({Consumer}, Context, "standalone-handler",
                               Arch::X86, {}, &F.Image, BinaryFormat::COFF);
    if (!ValidTable) {
      EXPECT_EQ(Module, nullptr);
      continue;
    }
    ASSERT_NE(Module, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto *Handler = Module->getFunction("__nd_registration_handler_4010C0");
    ASSERT_NE(Handler, nullptr);
    EXPECT_TRUE(Handler->isDeclaration());
    EXPECT_FALSE(Handler->use_empty());
    auto Identity = rewrite_source::getOriginalVA(*Handler);
    ASSERT_TRUE(bool(Identity));
    ASSERT_TRUE(Identity->has_value());
    EXPECT_EQ(**Identity, F.Handler);
  }
}

} // namespace
