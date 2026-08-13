//===- COFFExceptionPatchTests.cpp - Windows EH patch contract tests --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/Support/Error.h"

namespace {

using namespace neverd;

TEST(COFFExceptionPatch, AcceptsCompleteX64UnwindContract) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "sub_140001000";
  MedBlock Block;
  Block.Id = 0;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x20};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  Func.ExceptionMetadata = EH;

  llvm::LLVMContext Ctx;
  auto Module = MedLLVMEmitter().emit({Func}, Ctx, "eh-patch-safe", Arch::X64,
                                      {}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Image.ExceptionMetadata.rebuildIndex();

  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->ExceptionFunctionEntries.size(), 1u);
  EXPECT_EQ(Plan->ExceptionFunctionEntries[0], Func.Entry);

  Image.DynInfo.GuardFlags = 0x00800000u; // IMAGE_GUARD_XFG_ENABLED
  auto UnsupportedGuardPlan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(UnsupportedGuardPlan));
  EXPECT_NE(llvm::toString(UnsupportedGuardPlan.takeError())
                .find("guard instrumentation mode"),
            std::string::npos);
}

TEST(COFFExceptionPatch, ResolvesExecutablePersonalityThunkInsteadOfIATData) {
  BinaryImage Image;
  Image.Base = 0x140000000;
  Segment Code;
  Code.VA = Image.Base + 0x1000;
  Code.Size = 1;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = {0xc3};
  Image.Segments.push_back(std::move(Code));

  ExceptionFunction EH;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = Image.Base + 0x1000;
  Image.ExceptionMetadata.Functions.push_back(EH);

  EXPECT_EQ(findCOFFExceptionPersonalityVA(Image, "__C_specific_handler"),
            EH.PersonalityVA);
  EXPECT_EQ(findCOFFExceptionPersonalityVA(Image, "\01__C_specific_handler"),
            EH.PersonalityVA);
  EXPECT_FALSE(findCOFFExceptionPersonalityVA(Image, "__CxxFrameHandler3"));

  Image.Segments.front().Flags = SegmentFlags::Readable;
  EXPECT_FALSE(findCOFFExceptionPersonalityVA(Image, "__C_specific_handler"));
}

TEST(COFFExceptionPatch, RejectsLanguageGraphWithoutNativeWinEH) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "sub_140001000";
  MedBlock Block;
  Block.Id = 0;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x20};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.SEH.emplace();
  Func.ExceptionMetadata = EH;

  llvm::LLVMContext Ctx;
  auto Module = MedLLVMEmitter().emit({Func}, Ctx, "eh-patch-reject", Arch::X64,
                                      {}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Image.ExceptionMetadata.rebuildIndex();

  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(Plan));
  std::string Message = llvm::toString(Plan.takeError());
  EXPECT_NE(Message.find("native WinEH lowering is unavailable"),
            std::string::npos);
}

} // namespace
