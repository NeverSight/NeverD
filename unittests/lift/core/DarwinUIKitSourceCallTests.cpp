#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;
namespace {
constexpr auto UIKit = "/System/Library/Frameworks/UIKit.framework/UIKit";
constexpr auto FunctionName = "UIGraphicsBeginImageContextWithOptions";
constexpr auto ImportName = "_UIGraphicsBeginImageContextWithOptions";

BinaryImage image(const char *Import = ImportName) {
  BinaryImage I;
  I.Arch = Arch::AArch64;
  I.Format = BinaryFormat::MachO;
  I.Bits = Bitness::Bits64;
  I.DynInfo.NeededLibs = {UIKit};
  Segment S;
  S.VA = 0x1000;
  S.Size = S.FileSz = 0x1000;
  S.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  S.Data.resize(0x1000);
  const uint32_t Stub[] = {0xb0000010, 0xf940c210, 0xd61f0200};
  for (size_t J = 0; J != 3; ++J)
    llvm::support::endian::write32le(S.Data.data() + 0x100 + 4 * J, Stub[J]);
  I.Segments.push_back(S);
  Segment Storage;
  Storage.VA = 0x2000;
  Storage.FileOff = 0x1000;
  Storage.Size = Storage.FileSz = 0x1000;
  Storage.Flags = SegmentFlags::Readable;
  Storage.Data.resize(0x1000);
  I.Segments.push_back(Storage);
  Section Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  I.Sections.push_back(Text);
  Section Data;
  Data.VA = 0x2000;
  Data.FileOff = 0x1000;
  Data.Size = Data.FileSz = 0x1000;
  Data.Flags = SegmentFlags::Readable;
  I.Sections.push_back(Data);
  I.ImportPtrSlots[0x2180] = Import;
  EXPECT_TRUE(I.recordDyldBindSlot(0x2180, Import, 0, UIKit, false));
  return I;
}

TEST(DarwinUIKitSourceCalls, OptionsKeepSizeBoolAndScaleInIndependentBanks) {
  const auto Image = image();
  const auto H = darwinRuntimeSourceCallHint(Image, 0x2180);
  ASSERT_TRUE(H);
  EXPECT_EQ(H->TargetName, FunctionName);
  EXPECT_EQ(H->TargetAddress, 0x2180U);
  EXPECT_EQ(H->CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
  EXPECT_EQ(H->Signature.Origin, SourceFunctionTypeHint::OriginKind::DarwinSDK);
  EXPECT_EQ(H->Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(H->Signature.Parameters.size(), 3U);
  const auto &P = H->Signature.Parameters;
  ASSERT_EQ(P[0].Type->Kind, NdTypeKind::Struct);
  ASSERT_EQ(P[0].Type->Fields.size(), 2U);
  EXPECT_EQ(P[0].Type->Size, 16U);
  ASSERT_EQ(P[0].Components.size(), 2U);
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (size_t J = 0; J != 2; ++J) {
    EXPECT_EQ(P[0].Type->Fields[J]->Kind, NdTypeKind::Float);
    EXPECT_EQ(P[0].Type->Fields[J]->Size, 8U);
    EXPECT_EQ(P[0].Components[J].Kind, SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(P[0].Components[J].RegisterOffset, TRI.FPParamRegs[J]);
    EXPECT_EQ(P[0].Components[J].ValueBytes, 8U);
  }
  EXPECT_EQ(P[1].Type->Kind, NdTypeKind::Int);
  EXPECT_EQ(P[1].Type->Size, 1U);
  EXPECT_FALSE(P[1].Type->IsSigned);
  EXPECT_EQ(P[1].Location.Kind, SourceABICarrierKind::IntegerRegister);
  EXPECT_EQ(P[1].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(P[1].Location.ValueBytes, 1U);
  EXPECT_EQ(P[2].Type->Kind, NdTypeKind::Float);
  EXPECT_EQ(P[2].Type->Size, 8U);
  EXPECT_EQ(P[2].Location.Kind, SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(P[2].Location.RegisterOffset, TRI.FPParamRegs[2]);
  EXPECT_EQ(P[2].Location.ValueBytes, 8U);
  EXPECT_EQ(sourceABIParameters(H->Signature).size(), 4U);
  std::string Error;
  EXPECT_TRUE(validateSourceABI(H->Signature, Error)) << Error;
}

TEST(DarwinUIKitSourceCalls,
     FixedCallsRequireExactImportProviderAndArchitecture) {
  for (const auto Import :
       {ImportName, "_CGSizeFromString", "_UIAccessibilityPostNotification",
        "_UIAccessibilityAnnouncementNotification"}) {
    SCOPED_TRACE(Import);
    for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
      auto I = image(Import);
      switch (Mutation) {
      case 0:
        I.Arch = Arch::X64;
        break;
      case 1:
        I.DyldBindSlots.clear();
        break;
      case 2:
        I.DyldBindSlots.at(0x2180).Module = "/tmp/UIKit.framework/UIKit";
        break;
      case 3:
        I.DyldBindSlots.at(0x2180).Module =
            "/System/Library/Frameworks/UIKit.framework/Versions/A/UIKit";
        break;
      case 4:
        I.DyldBindSlots.at(0x2180).Module =
            "/System/Library/Frameworks/Foundation.framework/Foundation";
        break;
      case 5:
        I.DyldBindSlots.at(0x2180).Name = "_other";
        break;
      case 6:
        I.DyldBindSlots.at(0x2180).Addend = 8;
        break;
      case 7:
        I.DyldBindSlots.at(0x2180).WeakImport = true;
        break;
      case 8:
        I.ConflictingImportStorageSlots.insert(0x2180);
        break;
      case 9:
        I.IsRelocatable = true;
        break;
      case 10:
        I.Format = BinaryFormat::ELF;
        break;
      case 11:
        I.Bits = Bitness::Bits32;
        break;
      }
      if (llvm::StringRef(Import) == "_UIAccessibilityAnnouncementNotification")
        EXPECT_FALSE(darwinRuntimeGlobalAddressHint(I, 0x2180)) << Mutation;
      else
        EXPECT_FALSE(darwinRuntimeSourceCallHint(I, 0x2180)) << Mutation;
    }
  }
}

TEST(DarwinUIKitSourceCalls,
     AccessibilityNotificationKeepsItsUnsignedWordAndExternalStorage) {
  const auto I = image("_UIAccessibilityPostNotification");
  const auto H = darwinRuntimeSourceCallHint(I, 0x2180);
  ASSERT_TRUE(H);
  EXPECT_EQ(H->TargetName, "UIAccessibilityPostNotification");
  EXPECT_EQ(H->Signature.Origin, SourceFunctionTypeHint::OriginKind::DarwinSDK);
  EXPECT_EQ(H->Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(H->Signature.Parameters.size(), 2U);
  const auto &Notification = H->Signature.Parameters[0];
  EXPECT_EQ(Notification.Type->Kind, NdTypeKind::Int);
  EXPECT_EQ(Notification.Type->Size, 4U);
  EXPECT_FALSE(Notification.Type->IsSigned);
  EXPECT_EQ(Notification.Location.Kind, SourceABICarrierKind::IntegerRegister);
  EXPECT_EQ(Notification.Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Notification.Location.ValueBytes, 4U);
  const auto &Argument = H->Signature.Parameters[1];
  EXPECT_EQ(Argument.Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Argument.Type->Size, 8U);
  EXPECT_EQ(Argument.Location.RegisterOffset, a64reg::X1);
  EXPECT_EQ(Argument.Location.ValueBytes, 8U);
  EXPECT_EQ(sourceABIParameters(H->Signature).size(), 2U);
  const auto Storage = image("_UIAccessibilityAnnouncementNotification");
  const auto Address = darwinRuntimeGlobalAddressHint(Storage, 0x2180);
  ASSERT_TRUE(Address);
  EXPECT_EQ(Address->CallKind,
            SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress);
  EXPECT_EQ(Address->TargetName, "UIAccessibilityAnnouncementNotification");
  EXPECT_EQ(Address->TargetAddress, 0x2180U);
  EXPECT_TRUE(Address->Signature.Parameters.empty());
  EXPECT_EQ(Address->Signature.ReturnType->Kind, NdTypeKind::Ptr);
  EXPECT_FALSE(darwinRuntimeSourceCallHint(Storage, 0x2180));
  EXPECT_FALSE(darwinRuntimeGlobalAddressHint(I, 0x2180));
}

TEST(DarwinUIKitSourceCalls,
     SizeFromStringUsesTheSharedFloatingRecordResultABI) {
  const auto I = image("_CGSizeFromString");
  const auto H = darwinRuntimeSourceCallHint(I, 0x2180);
  ASSERT_TRUE(H);
  EXPECT_EQ(H->TargetName, "CGSizeFromString");
  EXPECT_EQ(H->Signature.Origin, SourceFunctionTypeHint::OriginKind::DarwinSDK);
  const auto &Signature = H->Signature;
  ASSERT_EQ(Signature.Parameters.size(), 1U);
  EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Signature.Parameters[0].Type->Size, 8U);
  EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Signature.Parameters[0].Location.ValueBytes, 8U);
  ASSERT_EQ(Signature.ReturnType->Kind, NdTypeKind::Struct);
  EXPECT_EQ(Signature.ReturnType->Size, 16U);
  ASSERT_EQ(Signature.ReturnType->Fields.size(), 2U);
  ASSERT_EQ(Signature.ReturnComponents.size(), 2U);
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (unsigned J = 0; J != 2; ++J) {
    EXPECT_EQ(Signature.ReturnType->Fields[J]->Kind, NdTypeKind::Float);
    EXPECT_EQ(Signature.ReturnType->Fields[J]->Size, 8U);
    EXPECT_EQ(Signature.ReturnComponents[J].Kind,
              SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Signature.ReturnComponents[J].RegisterOffset, TRI.FPParamRegs[J]);
    EXPECT_EQ(Signature.ReturnComponents[J].ValueBytes, 8U);
  }
  std::string Error;
  EXPECT_TRUE(validateSourceABI(Signature, Error)) << Error;
}

LowOp operation(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs,
                va_t Address) {
  LowOp O;
  O.Opcode = Code;
  O.Output = Output;
  O.Addr = Address;
  for (const auto &V : Inputs)
    O.addInput(V);
  return O;
}

void executeSource(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto FoundCompiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(FoundCompiler);
  const std::string Compiler = *FoundCompiler;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-uikit", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-uikit", "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-uikit", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code FileError;
  {
    llvm::raw_fd_ostream Out(SourcePath, FileError);
    ASSERT_FALSE(FileError);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef> Args{
        Compiler,  "-x",       "c",  "-std=gnu11", Optimization,
        "-Werror", SourcePath, "-o", BinaryPath};
    std::string Error;
    const auto Status = llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt,
                                                  Redirects, 30, 0, &Error);
    const auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "")
                         << Source;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                        Redirects, 30, 0, &Error),
              0)
        << Error << Source;
  }
}

TEST(DarwinUIKitSourceCalls, BoundOptionsConsumeAllFourScalarCarriers) {
  const auto I = image();
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  LowFunc F;
  F.Entry = 0x1200;
  F.Name = "begin_image_context";
  LowBlock B;
  B.Id = 0;
  B.StartAddr = F.Entry;
  B.EndAddr = 0x1220;
  B.Ops = {operation(NdOp::COPY, NdVar::reg(TRI.FPParamRegs[0], 8),
                     {NdVar::cst(0x402b000000000000ULL, 8)}, 0x1200), // 13.5
           operation(NdOp::COPY, NdVar::reg(TRI.FPParamRegs[1], 8),
                     {NdVar::cst(0x4045200000000000ULL, 8)}, 0x1204), // 42.25
           operation(NdOp::COPY, NdVar::reg(a64reg::X0, 4), {NdVar::cst(1, 4)},
                     0x1208),
           operation(NdOp::COPY, NdVar::reg(TRI.FPParamRegs[2], 8),
                     {NdVar::cst(0x4006000000000000ULL, 8)}, 0x120c), // 2.75
           operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1210),
           operation(NdOp::COPY, NdVar::reg(a64reg::X0, 4), {NdVar::cst(42, 4)},
                     0x1214),
           operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 4)}, 0x1218)};
  F.Blocks = {B};
  LowToMedConverter Converter;
  Converter.setBinaryImage(&I);
  Converter.setSourceCallHintsEnabled(true);
  auto Med = Converter.convert(F, Arch::AArch64, BinaryFormat::MachO);
  recoverCallAbi(Med, Arch::AArch64, {}, &I);
  bool Found = false;
  for (const auto &Block : Med.Blocks)
    for (const auto &O : Block.Ops)
      if (O.Opcode == NdOp::CALL) {
        Found = true;
        ASSERT_TRUE(O.SourceCallHint);
        EXPECT_EQ(O.NumInputs, 5U);
        EXPECT_EQ(O.Output.Size, 0U);
      }
  ASSERT_TRUE(Found);
  auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  unsigned Calls = 0;
  walkStmts(High.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (!E || E->Kind != ExprKind::Call)
        return;
      ++Calls;
      ASSERT_TRUE(E->SourceCallHint);
      ASSERT_EQ(E->Operands.size(), 3U);
      EXPECT_EQ(E->Operands[0]->Kind, ExprKind::Record);
      ASSERT_EQ(E->Operands[0]->Operands.size(), 2U);
      EXPECT_TRUE(sdk::objcSourceCallBound(*E, I, {}));
      auto Drift = std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
      Drift->Signature.Parameters[1].Type = NdType::makeInt(4, false);
      std::string Error;
      ASSERT_TRUE(
          assignDarwinFixedSourceABI(Drift->Signature, Arch::AArch64, Error));
      auto Changed = *E;
      Changed.SourceCallHint = Drift;
      EXPECT_FALSE(sdk::objcSourceCallBound(Changed, I, {}));
    });
  });
  ASSERT_EQ(Calls, 1U);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  EXPECT_NE(Source.find(FunctionName), std::string::npos) << Source;

  const auto Declared = darwinRuntimeSourceCallHint(I, 0x2180);
  ASSERT_TRUE(Declared);
  const auto SizeType = typeToC(Declared->Signature.Parameters[0].Type);
  Source += "\nstatic unsigned observed_calls;\nstatic int mismatch;\nvoid "
            "options_probe(" +
            SizeType +
            ", uint8_t, double) "
            "__asm__(\"_UIGraphicsBeginImageContextWithOptions\");\n"
            "void options_probe(" +
            SizeType +
            " size, uint8_t opaque, double scale) {\n"
            "++observed_calls; mismatch |= size.field_0 != 13.5 || "
            "size.field_1 != 42.25 || opaque != 1 || scale != 2.75;\n}\n"
            "int main(void) { return begin_image_context() != 42 || "
            "observed_calls != 1 || mismatch; }\n";
  executeSource(Source);
}

TEST(DarwinUIKitSourceCalls, TwoSizeResultsKeepEveryFieldAcrossTheSecondCall) {
  auto I = image("_CGSizeFromString");
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto D0 = NdVar::reg(TRI.FPParamRegs[0], 8);
  const auto D1 = NdVar::reg(TRI.FPParamRegs[1], 8);
  const auto D8 = NdVar::reg(a64reg::V(8), 8);
  const auto D9 = NdVar::reg(a64reg::V(9), 8);
  LowFunc F;
  F.Entry = 0x1200;
  F.Name = "size_pair";
  LowBlock B;
  B.Id = 0;
  B.StartAddr = F.Entry;
  B.Ops = {
      operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {NdVar::cst(0x1111, 8)},
                0x1200),
      operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1204),
      operation(NdOp::COPY, D8, {D0}, 0x1208),
      operation(NdOp::COPY, D9, {D1}, 0x120c),
      operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {NdVar::cst(0x2222, 8)},
                0x1210),
      operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1214),
      operation(NdOp::FLOAT_MULT, NdVar::tmp(0, 8),
                {D9, NdVar::cst(0x4024000000000000ULL, 8)}, 0x1218), // *10
      operation(NdOp::FLOAT_ADD, NdVar::tmp(1, 8), {D8, NdVar::tmp(0, 8)},
                0x121c),
      operation(NdOp::FLOAT_MULT, NdVar::tmp(2, 8),
                {D0, NdVar::cst(0x4059000000000000ULL, 8)}, 0x1220), // *100
      operation(NdOp::FLOAT_ADD, NdVar::tmp(3, 8),
                {NdVar::tmp(1, 8), NdVar::tmp(2, 8)}, 0x1224),
      operation(NdOp::FLOAT_MULT, NdVar::tmp(4, 8),
                {D1, NdVar::cst(0x408f400000000000ULL, 8)}, 0x1228), // *1000
      operation(NdOp::FLOAT_ADD, NdVar::tmp(5, 8),
                {NdVar::tmp(3, 8), NdVar::tmp(4, 8)}, 0x122c),
      operation(NdOp::FLOAT_TRUNC, NdVar::reg(a64reg::X0, 8),
                {NdVar::tmp(5, 8)}, 0x1230),
      operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, 0x1234)};
  B.EndAddr = 0x1238;
  F.Blocks = {B};
  LowToMedConverter Converter;
  Converter.setBinaryImage(&I);
  Converter.setSourceCallHintsEnabled(true);
  auto Med = Converter.convert(F, Arch::AArch64, BinaryFormat::MachO);
  recoverCallAbi(Med, Arch::AArch64, {}, &I);
  // This fixture's enclosing C function is int64_t(void); publication normally
  // supplies the enclosing method/native declaration before typed lowering.
  SourceFunctionTypeHint Entry;
  Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Entry.ReturnType = NdType::makeInt(8);
  std::string Error;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Entry, Arch::AArch64, Error));
  Med.SourceTypeHint = Entry;
  inferMedTypes(Med, Arch::AArch64);
  ASSERT_TRUE(Med.SourceTypeHint);
  const auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  unsigned Calls = 0;
  walkStmts(High.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (!E || E->Kind != ExprKind::Call)
        return;
      ++Calls;
      ASSERT_TRUE(E->SourceCallHint);
      ASSERT_EQ(E->Operands.size(), 1U);
      EXPECT_TRUE(sdk::objcSourceCallBound(*E, I, {}));
      for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
        auto Wrong = std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
        if (Mutation == 0)
          std::swap(Wrong->Signature.ReturnComponents[0],
                    Wrong->Signature.ReturnComponents[1]);
        else if (Mutation == 1)
          Wrong->Signature.ReturnComponents.pop_back();
        else {
          if (Mutation == 2)
            Wrong->Signature.ReturnType =
                NdType::makeStruct({NdType::makeInt(8), NdType::makeInt(8)});
          else
            Wrong->Signature.Parameters[0].Type = NdType::makeInt(8);
          std::string Error;
          ASSERT_TRUE(assignDarwinFixedSourceABI(Wrong->Signature,
                                                 Arch::AArch64, Error));
        }
        auto Changed = *E;
        Changed.SourceCallHint = Wrong;
        EXPECT_FALSE(sdk::objcSourceCallBound(Changed, I, {})) << Mutation;
      }
    });
  });
  ASSERT_EQ(Calls, 2U);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  const auto Declared = darwinRuntimeSourceCallHint(I, 0x2180);
  ASSERT_TRUE(Declared);
  const auto Size = typeToC(Declared->Signature.ReturnType);
  Source += "\nstatic unsigned observed_calls;\nstatic int mismatch;\n" + Size +
            " size_probe(void *) __asm__(\"_CGSizeFromString\");\n" + Size +
            " size_probe(void *value) {\n"
            "++observed_calls; mismatch |= (uintptr_t)value != "
            "(observed_calls == 1 ? 0x1111u : 0x2222u);\n" +
            Size +
            " result = {observed_calls == 1 ? 1.0 : 4.0, "
            "observed_calls == 1 ? 2.0 : 8.0}; return result; }\n"
            "int main(void) { return size_pair() != 8421 || "
            "observed_calls != 2 || mismatch; }\n";
  executeSource(Source);
}

TEST(DarwinUIKitSourceCalls,
     AnnouncementLoadsStayFourBytesAndNullableArgumentsReachTheRuntime) {
  auto I = image("_UIAccessibilityPostNotification");
  constexpr auto Announcement = "_UIAccessibilityAnnouncementNotification";
  I.ImportPtrSlots[0x2188] = Announcement;
  ASSERT_TRUE(I.recordDyldBindSlot(0x2188, Announcement, 0, UIKit, false));
  LowFunc F;
  F.Entry = 0x1200;
  F.Name = "post_accessibility";
  LowBlock B;
  B.Id = 0;
  B.StartAddr = F.Entry;
  const auto X8 = NdVar::reg(a64reg::X8, 8);
  const auto W0 = NdVar::reg(a64reg::X0, 4);
  const auto X1 = NdVar::reg(a64reg::X1, 8);
  B.Ops = {operation(NdOp::COPY, X8, {NdVar::cst(0x2188, 8)}, 0x1200),
           operation(NdOp::LOAD, X8, {X8}, 0x1204),
           operation(NdOp::LOAD, W0, {X8}, 0x1208),
           operation(NdOp::COPY, X1, {NdVar::cst(0x1234, 8)}, 0x120c),
           operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1210),
           operation(NdOp::COPY, X8, {NdVar::cst(0x2188, 8)}, 0x1214),
           operation(NdOp::LOAD, X8, {X8}, 0x1218),
           operation(NdOp::LOAD, W0, {X8}, 0x121c),
           operation(NdOp::COPY, X1, {NdVar::cst(0, 8)}, 0x1220),
           operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1224),
           operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {NdVar::cst(42, 8)},
                     0x1228),
           operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, 0x122c)};
  B.EndAddr = 0x1230;
  F.Blocks = {B};
  LowToMedConverter Converter;
  Converter.setBinaryImage(&I);
  Converter.setSourceCallHintsEnabled(true);
  auto Med = Converter.convert(F, Arch::AArch64, BinaryFormat::MachO);
  recoverCallAbi(Med, Arch::AArch64, {}, &I);
  SourceFunctionTypeHint Entry;
  Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Entry.ReturnType = NdType::makeInt(8);
  std::string Error;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Entry, Arch::AArch64, Error));
  Med.SourceTypeHint = Entry;
  inferMedTypes(Med, Arch::AArch64);
  auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  auto Bound = sdk::bindObjCSourceReferences(High, I);
  unsigned Calls = 0, StorageCalls = 0, WordLoads = 0;
  walkStmts(Bound.Function.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (!E)
        return;
      if (E->Kind == ExprKind::Load && E->Type && E->Type->Size == 4)
        ++WordLoads;
      if (E->Kind != ExprKind::Call)
        return;
      ASSERT_TRUE(E->SourceCallHint);
      EXPECT_TRUE(sdk::objcSourceCallBound(*E, I, {}));
      if (E->SourceCallHint->CallKind ==
          SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress) {
        ++StorageCalls;
        EXPECT_EQ(E->SourceCallHint->TargetAddress, 0x2188U);
        return;
      }
      ++Calls;
      ASSERT_EQ(E->Operands.size(), 2U);
      EXPECT_EQ(E->SourceCallHint->TargetName,
                "UIAccessibilityPostNotification");
      for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
        auto Wrong = std::make_shared<SourceCallTypeHint>(*E->SourceCallHint);
        if (Mutation == 0)
          Wrong->Signature.Parameters[0].Type = NdType::makeInt(1, false);
        else if (Mutation == 1)
          Wrong->Signature.Parameters[0].Type = NdType::makeInt(4, true);
        else if (Mutation == 2)
          Wrong->Signature.Parameters[0].Type = NdType::makeInt(8, false);
        else if (Mutation == 3)
          Wrong->Signature.Parameters[1].Type = NdType::makeInt(8, false);
        else
          Wrong->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
        ASSERT_TRUE(
            assignDarwinFixedSourceABI(Wrong->Signature, Arch::AArch64, Error));
        auto Changed = *E;
        Changed.SourceCallHint = Wrong;
        EXPECT_FALSE(sdk::objcSourceCallBound(Changed, I, {})) << Mutation;
      }
    });
  });
  ASSERT_EQ(Calls, 2U);
  EXPECT_GT(StorageCalls, 0U);
  EXPECT_EQ(WordLoads, 2U);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Bound.Function}, OS, Options));
  ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  Source +=
      "\nconst uint32_t announcement_probe "
      "__asm__(\"_UIAccessibilityAnnouncementNotification\") = 0xf1234567U;\n"
      "static unsigned observed_calls; static int mismatch;\n"
      "void post_probe(uint32_t, void *) "
      "__asm__(\"_UIAccessibilityPostNotification\");\n"
      "void post_probe(uint32_t notification, void *argument) {\n"
      " ++observed_calls; mismatch |= notification != 0xf1234567U || "
      "(uintptr_t)argument != (observed_calls == 1 ? 0x1234U : 0);\n}\n"
      "int main(void) { return post_accessibility() != 42 || observed_calls "
      "!= 2 || mismatch; }\n";
  executeSource(Source);
}
} // namespace
