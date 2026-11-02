#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/pipeline/Pipeline.h"

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
    // Decompiled C reads and writes scalars through plain pointer casts and
    // is built as it documents: without strict aliasing.
    llvm::SmallVector<llvm::StringRef> Args{
        Compiler,     "-x",         "c",
        "-std=gnu11", Optimization, "-fno-strict-aliasing",
        "-Werror",    SourcePath,   "-o",
        BinaryPath};
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

TEST(DarwinNativeRecordReturns, FourComputedDoublesExecuteAtO0AndO2) {
  SourceFunctionTypeHint Scalar;
  Scalar.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Scalar.ReturnType = NdType::makeFloat(8);
  Scalar.Parameters = {{"a", NdType::makeFloat(8)},
                       {"b", NdType::makeFloat(8)}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Scalar, Arch::AArch64, Error));
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "computed_quad";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Low.Entry;
  const NdOp Operations[] = {NdOp::FLOAT_ADD, NdOp::FLOAT_SUB, NdOp::FLOAT_MULT,
                             NdOp::FLOAT_DIV};
  for (unsigned I = 0; I != 4; ++I)
    Block.Ops.push_back(
        operation(Operations[I], NdVar::tmp(I * 8, 8),
                  {NdVar::reg(a64reg::V(0), 8), NdVar::reg(a64reg::V(1), 8)},
                  Low.Entry + I * 4));
  for (unsigned I = 0; I != 4; ++I)
    Block.Ops.push_back(operation(NdOp::INT_ZEXT, NdVar::reg(a64reg::V(I), 16),
                                  {NdVar::tmp(I * 8, 8)},
                                  Low.Entry + 16 + I * 4));
  Block.Ops.push_back(operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)},
                                Low.Entry + 32));
  Block.EndAddr = Low.Entry + 36;
  Low.Blocks = {Block};
  PipelineFunctionAudit Audit;
  Audit.Entry = Low.Entry;
  Audit.Disposition = PipelineFunctionDisposition::Accepted;
  Audit.HasLowIR = Audit.HasMedIR = Audit.MedIRVerified = true;
  Audit.DecodedInstructions = Audit.LiftedInstructions = 9;
  const auto Bind = [&](const SourceFunctionTypeHint &Hint) {
    const std::map<va_t, SourceFunctionTypeHint> Entries{{Low.Entry, Hint}};
    LowToMedConverter Converter;
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceEntryTypeHints(&Entries);
    auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
    Med.SourceTypeHint = Hint;
    recoverCallAbi(Med, Arch::AArch64, {});
    inferMedTypes(Med, Arch::AArch64);
    return Med;
  };
  auto Med = Bind(Scalar);
  auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  const auto Record = refineNativeFourDoubleReturnHint(Med, High, Audit);
  ASSERT_TRUE(Record);
  Med = Bind(*Record);
  High = MedToHighConverter().convert(Med, Arch::AArch64);
  ASSERT_TRUE(Med.SourceParametersBound);
  const auto Limitation = sdk::sourceBodyLimitation(High, *Record, &Audit);
  ASSERT_TRUE(Limitation.empty()) << Limitation;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  Source += "\n#include <string.h>\nint main(void) {\n"
            "  for (unsigned i = 0; i != 2048; ++i) {\n"
            "    double a = ((int)(i % 127) - 63) / 8.0;\n"
            "    double b = ((int)(i % 61) - 30) / 4.0;\n"
            "    if (b == 0) b = -0.5;\n"
            "    volatile double first = a, second = b;\n"
            "    double expected[4] = {first + second, first - second,\n"
            "                           first * second, first / second};\n"
            "    " +
            typeToC(Record->ReturnType) +
            " actual = computed_quad(a, b);\n"
            "    if (memcmp(&actual, expected, sizeof(expected))) return 1;\n"
            "  }\n  return 0;\n}\n";
  executeSource(Source);
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
  // A load may sit in the call that reads it, and a storage call in the
  // address of the load.
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E)
      return;
    for (const ExprPtr &Operand : E->Operands)
      Visit(Operand);
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
    EXPECT_EQ(E->SourceCallHint->TargetName, "UIAccessibilityPostNotification");
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
  };
  walkStmts(Bound.Function.Body,
            [&](const HighStmt &S) { forEachExpr(S, Visit); });
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
TEST(DarwinIndirectRecordCalls,
     ConcatCTMBridgesOnlyTheExactCoreGraphicsArm64Import) {
  constexpr auto CoreGraphics =
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
  auto I = image("_CGContextConcatCTM");
  I.DynInfo.NeededLibs = {CoreGraphics};
  I.DyldBindSlots.at(0x2180).Module = CoreGraphics;
  const auto Hint = darwinRuntimeSourceCallHint(I, 0x2180);
  ASSERT_TRUE(Hint);
  EXPECT_EQ(Hint->CallKind, SourceCallTypeHint::Kind::DarwinRuntimeCall);
  EXPECT_EQ(Hint->ByteCount, 48U);
  EXPECT_EQ(Hint->Signature.ReturnType->Kind, NdTypeKind::Void);
  ASSERT_EQ(Hint->Signature.Parameters.size(), 2U);
  EXPECT_EQ(Hint->Signature.Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Signature.Parameters[1].Location.RegisterOffset, a64reg::X1);
  EXPECT_EQ(Hint->Signature.Parameters[1].Type->Kind, NdTypeKind::Ptr);

  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Parameter = [&](unsigned Id) {
    MedVar V;
    V.Kind = MedVar::Param;
    V.Id = Id;
    V.Size = 8;
    V.TheArch = Arch::AArch64;
    return HighExpr::makeVar(V, Pointer);
  };
  auto Call = HighExpr::makeCall("CGContextConcatCTM", 0x1100,
                                 {Parameter(0), Parameter(1)});
  Call->Type = NdType::makeVoid();
  Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
  EXPECT_TRUE(sdk::objcSourceCallBound(*Call, I, {}));
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Wrong = std::make_shared<SourceCallTypeHint>(*Hint);
    if (Mutation == 0)
      Wrong->ByteCount = 47;
    else if (Mutation == 1)
      Wrong->Signature.Parameters[1].Type = NdType::makeInt(8);
    else if (Mutation == 2)
      Wrong->Signature.ReturnType = Pointer;
    else
      Wrong->Signature.Parameters[1].Location.RegisterOffset = a64reg::X2;
    auto Changed = *Call;
    Changed.SourceCallHint = Wrong;
    EXPECT_FALSE(sdk::objcSourceCallBound(Changed, I, {})) << Mutation;
  }
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Changed = I;
    if (Mutation == 0)
      Changed.Arch = Arch::X64;
    else if (Mutation == 1)
      Changed.DyldBindSlots.at(0x2180).Module =
          "/System/Library/Frameworks/UIKit.framework/UIKit";
    else if (Mutation == 2)
      Changed.DyldBindSlots.at(0x2180).WeakImport = true;
    else if (Mutation == 3)
      Changed.DyldBindSlots.at(0x2180).Addend = 8;
    else
      Changed.IsRelocatable = true;
    EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
  }

  HighFunc F;
  F.Entry = 0x1200;
  F.Name = "concat_ctm";
  F.Params = {{"context", Pointer}, {"transform", Pointer}};
  F.ReturnType = NdType::makeVoid();
  F.SourceTypeHint = Hint->Signature;
  HighStmt Effect;
  Effect.Kind = StmtKind::Call;
  Effect.CallExpr = Call;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  F.Body = {Effect, Return};
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({F}, OS, Options));
  ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  ASSERT_NE(Source.find("memcpy(&value, transform, sizeof(value))"),
            std::string::npos)
      << Source;
  Source += R"(
static unsigned observed_calls;
static int mismatch;
void coregraphics_probe(void *, neverd_CGAffineTransform)
    __asm__("_CGContextConcatCTM");
void coregraphics_probe(void *context, neverd_CGAffineTransform value) {
  ++observed_calls;
  mismatch |= (uintptr_t)context != 0x1234 || value.a != 1.0 ||
              value.b != 2.0 || value.c != 3.0 || value.d != 4.0 ||
              value.tx != 5.0 || value.ty != 6.0;
}
int main(void) {
  double values[6] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  concat_ctm((void *)(uintptr_t)0x1234, values);
  return observed_calls != 1 || mismatch;
}
)";
  executeSource(Source);
}

TEST(DarwinIndirectRecordCalls,
     AffineOperationsKeepTheInputPointerAndEveryIndirectResultField) {
  constexpr auto CoreGraphics =
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
  for (const auto *Name :
       {"CGAffineTransformTranslate", "CGAffineTransformScale",
        "CGAffineTransformRotate"}) {
    SCOPED_TRACE(Name);
    const std::string Import = "_" + std::string(Name);
    auto I = image(Import.c_str());
    I.DynInfo.NeededLibs = {CoreGraphics};
    I.DyldBindSlots.at(0x2180).Module = CoreGraphics;
    const auto Hint = darwinRuntimeSourceCallHint(I, 0x2180);
    ASSERT_TRUE(Hint);
    const bool Rotate = std::string(Name) == "CGAffineTransformRotate";
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Hint->ByteCount, 48U);
    ASSERT_EQ(Signature.ReturnType->Kind, NdTypeKind::Struct);
    EXPECT_EQ(Signature.ReturnType->Size, 48U);
    ASSERT_EQ(Signature.Parameters.size(), Rotate ? 2U : 3U);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset, a64reg::X0);
    EXPECT_EQ(Signature.ReturnLocation.Kind,
              SourceABICarrierKind::IndirectResultPointer);
    EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, a64reg::X8);
    const auto &TRI = getTargetRegInfo(Arch::AArch64);
    for (unsigned J = 1; J < Signature.Parameters.size(); ++J) {
      EXPECT_EQ(Signature.Parameters[J].Type->Kind, NdTypeKind::Float);
      EXPECT_EQ(Signature.Parameters[J].Type->Size, 8U);
      EXPECT_EQ(Signature.Parameters[J].Location.RegisterOffset,
                TRI.FPParamRegs[J - 1]);
    }
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      auto Changed = I;
      if (Mutation == 0)
        Changed.Arch = Arch::X64;
      else if (Mutation == 1)
        Changed.DyldBindSlots.at(0x2180).Module =
            "/System/Library/Frameworks/UIKit.framework/UIKit";
      else if (Mutation == 2)
        Changed.DyldBindSlots.at(0x2180).WeakImport = true;
      else if (Mutation == 3)
        Changed.DyldBindSlots.at(0x2180).Addend = 8;
      else
        Changed.IsRelocatable = true;
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
    }

    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    const auto Double = NdType::makeFloat(8);
    SourceFunctionTypeHint Entry;
    Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Entry.ReturnType = NdType::makeInt(8, false);
    Entry.Parameters = {{"input", Pointer},
                        {"output", Pointer},
                        {"tx", Double},
                        {"ty", Double}};
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Arch::AArch64, Error));
    LowFunc F;
    F.Entry = 0x1200;
    F.Name = "apply_affine";
    LowBlock B;
    B.Id = 0;
    B.StartAddr = F.Entry;
    B.EndAddr = 0x1218;
    B.Ops = {operation(NdOp::COPY, NdVar::reg(a64reg::X19, 8),
                       {NdVar::reg(a64reg::X1, 8)}, 0x1200),
             operation(NdOp::COPY, NdVar::reg(a64reg::X8, 8),
                       {NdVar::reg(a64reg::X19, 8)}, 0x1204),
             operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1208),
             operation(NdOp::COPY, NdVar::reg(a64reg::X8, 8),
                       {NdVar::cst(0, 8)}, 0x120c),
             operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                       {NdVar::cst(0, 8)}, 0x1210),
             operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, 0x1214)};
    F.Blocks = {B};
    const std::map<va_t, SourceFunctionTypeHint> Entries{{F.Entry, Entry}};
    LowToMedConverter Converter;
    Converter.setBinaryImage(&I);
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceCalleeTypeHints(&Entries);
    auto Med = Converter.convert(F, Arch::AArch64, BinaryFormat::MachO);
    Med.SourceTypeHint = Entry;
    recoverCallAbi(Med, Arch::AArch64, {}, &I);
    inferMedTypes(Med, Arch::AArch64);
    unsigned ResultStores = 0;
    for (const auto &Block : Med.Blocks)
      for (const auto &O : Block.Ops)
        if (O.Opcode == NdOp::STORE && O.Addr == 0x1208)
          ++ResultStores;
    EXPECT_EQ(ResultStores, 6U);
    auto High = MedToHighConverter().convert(Med, Arch::AArch64);
    unsigned Calls = 0;
    walkStmts(High.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        ASSERT_FALSE(E && E->Kind == ExprKind::Undef);
        if (!E || E->Kind != ExprKind::Call)
          return;
        ++Calls;
        ASSERT_TRUE(E->SourceCallHint);
        EXPECT_EQ(E->Operands.size(), Rotate ? 2U : 3U);
        EXPECT_TRUE(sdk::objcSourceCallBound(*E, I, {}));
        for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
          auto Wrong = std::make_shared<SourceCallTypeHint>(*Hint);
          if (Mutation == 0)
            Wrong->ByteCount = 47;
          else if (Mutation == 1)
            Wrong->Signature.Parameters[1].Type = NdType::makeInt(8);
          else if (Mutation == 2)
            Wrong->Signature.ReturnLocation.RegisterOffset = a64reg::X9;
          else
            Wrong->Signature.Parameters[0].Location.RegisterOffset = a64reg::X1;
          auto Changed = *E;
          Changed.SourceCallHint = Wrong;
          EXPECT_FALSE(sdk::objcSourceCallBound(Changed, I, {})) << Mutation;
        }
      });
    });
    ASSERT_EQ(Calls, 1U);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    const auto Record = typeToC(Signature.ReturnType);
    Source += "\nstatic unsigned calls; static int mismatch;\n" + Record +
              " affine_probe(" + Record + ", double" +
              (Rotate ? "" : ", double") + ") __asm__(\"_" + Name + "\");\n" +
              Record + " affine_probe(" + Record + " input, double tx" +
              (Rotate ? "" : ", double ty") +
              ") {\n  ++calls; double values[6]; memcpy(values, &input, 48);\n"
              "  for (unsigned j = 0; j != 6; ++j) mismatch |= values[j] != "
              "(double)(j + 1);\n  mismatch |= tx != 2.5" +
              (Rotate ? "" : " || ty != -3.5") +
              ";\n  for (unsigned j = 0; j != 6; ++j) values[j] += tx * "
              "(j + 1)" +
              (Rotate ? "" : " + ty") + ";\n  " + Record +
              " result; memcpy(&result, values, 48); return result;\n}\n"
              "int main(void) {\n  double input[6] = {1, 2, 3, 4, 5, 6};\n"
              "  double output[7] = {-99, -99, -99, -99, -99, -99, 123};\n"
              "  if (apply_affine(input, output, 2.5, -3.5) != 0) return 1;\n"
              "  for (unsigned j = 0; j != 6; ++j) if (output[j] != "
              "(double)(j + 1) * 3.5" +
              (Rotate ? "" : " - 3.5") +
              ") return 2;\n  return calls != 1 || mismatch || "
              "output[6] != 123;\n}\n";
    executeSource(Source);
  }
}
TEST(DarwinIndirectRecordCalls,
     AffineConcatSnapshotsBothInputsBeforeWritingAnAliasedResult) {
  constexpr auto CoreGraphics =
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
  auto I = image("_CGAffineTransformConcat");
  I.DynInfo.NeededLibs = {CoreGraphics};
  I.DyldBindSlots.at(0x2180).Module = CoreGraphics;
  const auto Hint = darwinRuntimeSourceCallHint(I, 0x2180);
  ASSERT_TRUE(Hint);
  const auto &Signature = Hint->Signature;
  EXPECT_EQ(Hint->ByteCount, 48U);
  ASSERT_EQ(Signature.ReturnType->Kind, NdTypeKind::Struct);
  EXPECT_EQ(Signature.ReturnType->Size, 48U);
  ASSERT_EQ(Signature.Parameters.size(), 2U);
  for (unsigned J = 0; J != 2; ++J) {
    EXPECT_EQ(Signature.Parameters[J].Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.Parameters[J].Location.Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(Signature.Parameters[J].Location.RegisterOffset, J * 8U);
  }
  EXPECT_EQ(Signature.ReturnLocation.Kind,
            SourceABICarrierKind::IndirectResultPointer);
  EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, a64reg::X8);
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Changed = I;
    if (Mutation == 0)
      Changed.Arch = Arch::X64;
    else if (Mutation == 1)
      Changed.DyldBindSlots.at(0x2180).Module =
          "/System/Library/Frameworks/UIKit.framework/UIKit";
    else if (Mutation == 2)
      Changed.DyldBindSlots.at(0x2180).WeakImport = true;
    else if (Mutation == 3)
      Changed.DyldBindSlots.at(0x2180).Addend = 8;
    else
      Changed.IsRelocatable = true;
    EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
  }

  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  SourceFunctionTypeHint Entry;
  Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Entry.ReturnType = NdType::makeInt(8, false);
  Entry.Parameters = {
      {"first", Pointer}, {"second", Pointer}, {"output", Pointer}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Arch::AArch64, Error));
  LowFunc F;
  F.Entry = 0x1200;
  F.Name = "concat_affine";
  LowBlock B;
  B.Id = 0;
  B.StartAddr = F.Entry;
  B.EndAddr = 0x1218;
  B.Ops = {operation(NdOp::COPY, NdVar::reg(a64reg::X8, 8),
                     {NdVar::reg(a64reg::X2, 8)}, 0x1200),
           operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1204),
           operation(NdOp::COPY, NdVar::reg(a64reg::X8, 8), {NdVar::cst(0, 8)},
                     0x1208),
           operation(NdOp::COPY, NdVar::reg(a64reg::X1, 8), {NdVar::cst(0, 8)},
                     0x120c),
           operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {NdVar::cst(0, 8)},
                     0x1210),
           operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, 0x1214)};
  F.Blocks = {B};
  const std::map<va_t, SourceFunctionTypeHint> Entries{{F.Entry, Entry}};
  LowToMedConverter Converter;
  Converter.setBinaryImage(&I);
  Converter.setSourceCallHintsEnabled(true);
  Converter.setSourceCalleeTypeHints(&Entries);
  auto Med = Converter.convert(F, Arch::AArch64, BinaryFormat::MachO);
  Med.SourceTypeHint = Entry;
  recoverCallAbi(Med, Arch::AArch64, {}, &I);
  inferMedTypes(Med, Arch::AArch64);
  unsigned ResultStores = 0;
  for (const auto &Block : Med.Blocks)
    for (const auto &O : Block.Ops)
      if (O.Opcode == NdOp::STORE && O.Addr == 0x1204)
        ++ResultStores;
  EXPECT_EQ(ResultStores, 6U);
  auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  unsigned Calls = 0;
  walkStmts(High.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      ASSERT_FALSE(E && E->Kind == ExprKind::Undef);
      if (!E || E->Kind != ExprKind::Call)
        return;
      ++Calls;
      ASSERT_TRUE(E->SourceCallHint);
      EXPECT_EQ(E->Operands.size(), 2U);
      EXPECT_TRUE(sdk::objcSourceCallBound(*E, I, {}));
      for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
        auto Wrong = std::make_shared<SourceCallTypeHint>(*Hint);
        if (Mutation == 0)
          Wrong->ByteCount = 47;
        else if (Mutation == 1)
          Wrong->Signature.Parameters[1].Type = NdType::makeFloat(8);
        else if (Mutation == 2)
          Wrong->Signature.ReturnLocation.RegisterOffset = a64reg::X9;
        else
          Wrong->Signature.Parameters[1].Location.RegisterOffset = a64reg::X2;
        auto Changed = *E;
        Changed.SourceCallHint = Wrong;
        EXPECT_FALSE(sdk::objcSourceCallBound(Changed, I, {})) << Mutation;
      }
    });
  });
  ASSERT_EQ(Calls, 1U);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  const auto Record = typeToC(Signature.ReturnType);
  Source += "\nstatic unsigned calls; static int mismatch;\n" + Record +
            " concat_probe(" + Record + ", " + Record +
            ") __asm__(\"_CGAffineTransformConcat\");\n" + Record +
            " concat_probe(" + Record + " first, " + Record +
            " second) {\n  ++calls; double a[6], b[6];\n"
            "  memcpy(a, &first, 48); memcpy(b, &second, 48);\n"
            "  for (unsigned j = 0; j != 6; ++j) {\n"
            "    mismatch |= a[j] != j + 1 || b[j] != j + 11;\n"
            "    a[j] = a[j] * 3 + b[5 - j] * 7;\n  }\n  " +
            Record +
            " result; memcpy(&result, a, 48); return result;\n}\n"
            "int main(void) {\n  double first[7] = {1,2,3,4,5,6,123};\n"
            "  double second[7] = {11,12,13,14,15,16,456};\n"
            "  double output[7] = {0,0,0,0,0,0,789};\n"
            "  if (concat_affine(first, second, output) != 0) return 1;\n"
            "  for (unsigned j = 0; j != 6; ++j)\n"
            "    if (output[j] != (j + 1) * 3 + (16 - j) * 7) return 2;\n"
            "  if (concat_affine(first, second, second) != 0) return 3;\n"
            "  for (unsigned j = 0; j != 6; ++j)\n"
            "    if (second[j] != (j + 1) * 3 + (16 - j) * 7) return 4;\n"
            "  return calls != 2 || mismatch || first[6] != 123 || "
            "second[6] != 456 || output[6] != 789;\n}\n";
  executeSource(Source);
}

void checkAffineInputSnapshot(const char *Name, bool Translation) {
  constexpr auto CoreGraphics =
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
  auto Image = image(("_" + std::string(Name)).c_str());
  Image.DynInfo.NeededLibs = {CoreGraphics};
  Image.DyldBindSlots.at(0x2180).Module = CoreGraphics;
  const auto Hint = darwinRuntimeSourceCallHint(Image, 0x2180);
  ASSERT_TRUE(Hint);
  const auto Effects = darwinMatrixSourceFrameEffects(Image, *Hint);
  ASSERT_TRUE(Effects);
  ASSERT_EQ(Hint->Signature.Parameters.size(), Translation ? 3U : 1U);
  EXPECT_EQ(Hint->Signature.Parameters[0].Location.RegisterOffset, a64reg::X0);
  EXPECT_EQ(Hint->Signature.ReturnLocation.RegisterOffset, a64reg::X8);
  EXPECT_EQ(Hint->ByteCount, 48U);
  EXPECT_EQ(Effects->WritableFrameParameters,
            (std::map<size_t, size_t>{{0, 48}}));
  EXPECT_EQ(Effects->InitializedFrameParameters, (std::set<size_t>{0}));
  EXPECT_TRUE(Effects->InitializesIndirectResult);
  const auto Double = NdType::makeFloat(8);
  if (Translation) {
    for (unsigned J = 0; J != 2; ++J) {
      const auto &Parameter = Hint->Signature.Parameters[J + 1];
      EXPECT_TRUE(equalSourceTypes(Parameter.Type, Double));
      EXPECT_EQ(Parameter.Location.Kind,
                SourceABICarrierKind::FloatingRegister);
      EXPECT_EQ(Parameter.Location.RegisterOffset,
                getTargetRegInfo(Arch::AArch64).FPParamRegs[J]);
    }
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      auto Changed = *Hint;
      if (Mutation == 0)
        Changed.Signature.Parameters[1].Location.RegisterOffset = a64reg::X1;
      else if (Mutation == 1)
        Changed.Signature.Parameters[1].Location.ValueBytes = 4;
      else if (Mutation == 2)
        Changed.Signature.Parameters[2].Location =
            Changed.Signature.Parameters[1].Location;
      else
        Changed.Signature.Parameters[2].Type = NdType::makeFloat(4);
      EXPECT_FALSE(darwinMatrixSourceFrameEffects(Image, Changed)) << Mutation;
    }
  }
  SourceFunctionTypeHint Entry;
  Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Entry.ReturnType = NdType::makeVoid();
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  Entry.Parameters = {{"input", Pointer}, {"output", Pointer}};
  if (Translation) {
    Entry.Parameters.push_back({"first", Double});
    Entry.Parameters.push_back({"second", Double});
  }
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Arch::AArch64, Error));
  LowFunc Low;
  Low.Entry = 0x1200;
  Low.Name = "snapshot_affine";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Low.Entry;
  Block.EndAddr = Low.Entry + 12;
  Block.Ops = {
      operation(NdOp::COPY, NdVar::reg(a64reg::X8, 8),
                {NdVar::reg(a64reg::X1, 8)}, Low.Entry),
      operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, Low.Entry + 4),
      operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)}, Low.Entry + 8)};
  Low.Blocks = {Block};
  const std::map<va_t, SourceFunctionTypeHint> Entries{{Low.Entry, Entry}};
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  Converter.setSourceCallHintsEnabled(true);
  Converter.setSourceEntryTypeHints(&Entries);
  auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
  Med.SourceTypeHint = Entry;
  recoverCallAbi(Med, Arch::AArch64, {}, &Image);
  inferMedTypes(Med, Arch::AArch64);
  const auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  unsigned Calls = 0, Stores = 0;
  walkStmts(High.Body, [&](const HighStmt &Statement) {
    Stores += Statement.Kind == StmtKind::Store;
    forEachExpr(Statement, [&](const ExprPtr &Expression) {
      if (Expression && Expression->Kind == ExprKind::Call) {
        ++Calls;
        EXPECT_TRUE(sdk::objcSourceCallBound(*Expression, Image, {}));
      }
    });
  });
  EXPECT_EQ(Calls, 1U);
  EXPECT_EQ(Stores, 6U);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  const auto Record = typeToC(Hint->Signature.ReturnType);
  const std::string ScalarDeclaration = Translation ? ", double, double" : "";
  const std::string ScalarDefinition =
      Translation ? ", double first, double second" : "";
  const std::string ScalarCheck =
      Translation ? "  uint64_t a, b; memcpy(&a, &first, 8); "
                    "memcpy(&b, &second, 8);\n"
                    "  mismatch |= a != scalar_bits[0] || "
                    "b != scalar_bits[1];\n"
                  : "";
  const std::string ResultBits =
      "(0x123456789abcdef0ULL + j)" +
      std::string(Translation ? " ^ scalar_bits[j % 2]" : "");
  const std::string CallScalars = Translation ? ", scalars[0], scalars[1]" : "";
  Source += "\n#include <string.h>\n"
            "static uint64_t input_bits[6], scalar_bits[2]; "
            "static unsigned calls, mismatch;\n" +
            Record + " snapshot_probe(" + Record + ScalarDeclaration +
            ") __asm__(\"_" + Name + "\");\n" + Record + " snapshot_probe(" +
            Record + " input" + ScalarDefinition + ") {\n" + ScalarCheck +
            "  ++calls; mismatch |= memcmp(&input, input_bits, 48) != 0;\n"
            "  uint64_t bits[6]; memcpy(bits, &input, 48);\n"
            "  for (unsigned j = 0; j != 6; ++j) bits[j] ^= " +
            ResultBits + ";\n  " + Record +
            " result; memcpy(&result, bits, 48); return result;\n}\n"
            "int main(void) {\n"
            "  static const uint64_t cases[] = {0, 0x8000000000000000ULL, "
            "0x3ff0000000000000ULL, 0x7ff0000000000000ULL, "
            "0xfff0000000000000ULL, 0x7ff8000000001234ULL, 1, "
            "0x8000000000000001ULL};\n"
            "  static const unsigned outputs[] = {1, 3, 4, 5, 9};\n"
            "  for (unsigned i = 0; i != 512; ++i) {\n"
            "    scalar_bits[0] = cases[i % 8]; "
            "scalar_bits[1] = cases[(i / 8) % 8];\n"
            "    double scalars[2]; memcpy(scalars, scalar_bits, 16);\n"
            "    for (unsigned j = 0; j != 6; ++j) input_bits[j] = "
            "cases[(i + j) % 8] ^ ((uint64_t)(i / 8) << 8) ^ j;\n"
            "    for (unsigned k = 0; k != 5; ++k) {\n"
            "      uint64_t storage[16], expected[16];\n"
            "      memset(storage, 0xa5, sizeof(storage));\n"
            "      memcpy(storage + 3, input_bits, 48);\n"
            "      memcpy(expected, storage, sizeof(storage));\n"
            "      for (unsigned j = 0; j != 6; ++j) "
            "expected[outputs[k] + j] = input_bits[j] ^ " +
            ResultBits +
            ";\n"
            "      snapshot_affine(storage + 3, storage + outputs[k]" +
            CallScalars +
            ");\n"
            "      if (memcmp(storage, expected, sizeof(storage))) return 1;\n"
            "    }\n  }\n  return mismatch || calls != 2560;\n}\n";
  executeSource(Source);
}

TEST(DarwinIndirectRecordCalls, AffineInvertSnapshotsItsCompleteAliasedInput) {
  checkAffineInputSnapshot("CGAffineTransformInvert", false);
}

TEST(DarwinIndirectRecordCalls,
     AffineTranslatePreservesScalarBitsAndSnapshotsAliasedInput) {
  checkAffineInputSnapshot("CGAffineTransformTranslate", true);
}

TEST(DarwinIndirectRecordCalls,
     MatrixConstructorsWriteAllSixteenFieldsWithExactScalarArguments) {
  constexpr auto QuartzCore =
      "/System/Library/Frameworks/QuartzCore.framework/QuartzCore";
  const auto Double = NdType::makeFloat(8);
  for (const auto *Name :
       {"CATransform3DMakeTranslation", "CATransform3DMakeScale",
        "CATransform3DMakeRotation"}) {
    SCOPED_TRACE(Name);
    const bool Rotation = std::string(Name) == "CATransform3DMakeRotation";
    const unsigned ArgumentCount = Rotation ? 4 : 3;
    const std::string Import = "_" + std::string(Name);
    auto I = image(Import.c_str());
    I.DynInfo.NeededLibs = {QuartzCore};
    I.DyldBindSlots.at(0x2180).Module = QuartzCore;
    const auto Hint = darwinRuntimeSourceCallHint(I, 0x2180);
    ASSERT_TRUE(Hint);
    const auto &Signature = Hint->Signature;
    EXPECT_EQ(Signature.ReturnType->Size, 128U);
    EXPECT_EQ(sourceAggregateMembers(Signature.ReturnType).size(), 16U);
    EXPECT_EQ(Signature.ReturnLocation.Kind,
              SourceABICarrierKind::IndirectResultPointer);
    EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, a64reg::X8);
    ASSERT_EQ(Signature.Parameters.size(), ArgumentCount);
    const auto &TRI = getTargetRegInfo(Arch::AArch64);
    for (unsigned J = 0; J != ArgumentCount; ++J) {
      EXPECT_TRUE(equalSourceTypes(Signature.Parameters[J].Type, Double));
      EXPECT_EQ(Signature.Parameters[J].Location.Kind,
                SourceABICarrierKind::FloatingRegister);
      EXPECT_EQ(Signature.Parameters[J].Location.RegisterOffset,
                TRI.FPParamRegs[J]);
    }
    for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
      auto Changed = I;
      if (Mutation == 0)
        Changed.Arch = Arch::X64;
      else if (Mutation == 1)
        Changed.DyldBindSlots.at(0x2180).Module =
            "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
      else if (Mutation == 2)
        Changed.DyldBindSlots.at(0x2180).Addend = 8;
      else if (Mutation == 3)
        Changed.IsRelocatable = true;
      else if (Mutation == 4)
        Changed.Bits = Bitness::Bits32;
      else
        Changed.ImportPtrSlots[0x2180] = "_CATransform3DScale";
      EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
    }

    SourceFunctionTypeHint Entry;
    Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Entry.ReturnType = NdType::makeInt(8, false);
    Entry.Parameters = {{"output", NdType::makePtr(NdType::makeVoid())},
                        {"first", Double},
                        {"second", Double},
                        {"third", Double}};
    if (Rotation)
      Entry.Parameters.push_back({"fourth", Double});
    std::string Error;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Arch::AArch64, Error));
    LowFunc F;
    F.Entry = 0x1200;
    F.Name = "construct_matrix";
    LowBlock B;
    B.Id = 0;
    B.StartAddr = F.Entry;
    B.EndAddr = 0x1214;
    B.Ops = {operation(NdOp::COPY, NdVar::reg(a64reg::X8, 8),
                       {NdVar::reg(a64reg::X0, 8)}, 0x1200),
             operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1204),
             operation(NdOp::COPY, NdVar::reg(a64reg::X8, 8),
                       {NdVar::cst(0, 8)}, 0x1208),
             operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                       {NdVar::cst(0, 8)}, 0x120c),
             operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, 0x1210)};
    F.Blocks = {B};
    const std::map<va_t, SourceFunctionTypeHint> Entries{{F.Entry, Entry}};
    LowToMedConverter Converter;
    Converter.setBinaryImage(&I);
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceCalleeTypeHints(&Entries);
    auto Med = Converter.convert(F, Arch::AArch64, BinaryFormat::MachO);
    Med.SourceTypeHint = Entry;
    recoverCallAbi(Med, Arch::AArch64, {}, &I);
    inferMedTypes(Med, Arch::AArch64);
    unsigned Stores = 0;
    for (const auto &Block : Med.Blocks)
      for (const auto &Op : Block.Ops)
        Stores += Op.Opcode == NdOp::STORE && Op.Addr == 0x1204;
    EXPECT_EQ(Stores, 16U);
    const auto High = MedToHighConverter().convert(Med, Arch::AArch64);
    unsigned Calls = 0;
    walkStmts(High.Body, [&](const HighStmt &S) {
      forEachExpr(S, [&](const ExprPtr &E) {
        ASSERT_FALSE(E && E->Kind == ExprKind::Undef);
        if (!E || E->Kind != ExprKind::Call)
          return;
        ++Calls;
        ASSERT_TRUE(E->SourceCallHint);
        EXPECT_EQ(E->Operands.size(), ArgumentCount);
        EXPECT_TRUE(sdk::objcSourceCallBound(*E, I, {}));
        auto Invalid = *E;
        auto Wrong = std::make_shared<SourceCallTypeHint>(*Hint);
        Wrong->Signature.ReturnLocation.RegisterOffset = a64reg::X9;
        Invalid.SourceCallHint = Wrong;
        EXPECT_FALSE(sdk::objcSourceCallBound(Invalid, I, {}));
        auto WrongProvider = I;
        WrongProvider.DyldBindSlots.at(0x2180).Module = "/tmp/QuartzCore";
        EXPECT_FALSE(sdk::objcSourceCallBound(*E, WrongProvider, {}));
      });
    });
    ASSERT_EQ(Calls, 1U);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::AArch64;
    ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
    ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    const auto Record = typeToC(Signature.ReturnType);
    Source +=
        "\nstatic unsigned calls; static int mismatch;\n"
        "static uint64_t payload[16];\n" +
        Record + " matrix_probe(double, double, double" +
        (Rotation ? ", double" : "") + ") __asm__(\"_" + Name + "\");\n" +
        Record + " matrix_probe(double first, double second, double third" +
        (Rotation ? ", double fourth" : "") +
        ") {\n  ++calls; mismatch |= first != 1.25 || "
        "second != -2.5 || third != 3.75" +
        (Rotation ? " || fourth != -4.5" : "") + ";\n  " + Record +
        " result; memcpy(&result, payload, sizeof(result)); "
        "return result;\n}\n"
        "int main(void) {\n"
        "  static const uint64_t cases[] = {0, 0x8000000000000000ULL, "
        "0x3ff0000000000000ULL, 0x7ff0000000000000ULL, "
        "0xfff0000000000000ULL, 0x7ff8000000001234ULL, 1, "
        "0x8000000000000001ULL};\n"
        "  for (unsigned i = 0; i != 512; ++i) {\n"
        "    for (unsigned j = 0; j != 16; ++j)\n"
        "      payload[j] = cases[(i + j) % 8] ^ "
        "((uint64_t)(i / 8) << 8) ^ j;\n"
        "    uint64_t storage[18]; memset(storage, 0xa5, sizeof(storage));\n"
        "    if (construct_matrix(storage + 1, 1.25, -2.5, 3.75" +
        (Rotation ? ", -4.5" : "") +
        ") != 0) return 1;\n"
        "    if (memcmp(storage + 1, payload, sizeof(payload))) return 2;\n"
        "    if (storage[0] != 0xa5a5a5a5a5a5a5a5ULL || "
        "storage[17] != storage[0] || calls != i + 1) return 3;\n"
        "  }\n  return mismatch;\n}\n";
    executeSource(Source);
  }
}

TEST(DarwinIndirectRecordCalls,
     MatrixScaleSnapshotsAllFieldsBeforeWritingAnAliasedResult) {
  constexpr auto QuartzCore =
      "/System/Library/Frameworks/QuartzCore.framework/QuartzCore";
  auto I = image("_CATransform3DScale");
  I.DynInfo.NeededLibs = {QuartzCore};
  I.DyldBindSlots.at(0x2180).Module = QuartzCore;
  const auto Hint = darwinRuntimeSourceCallHint(I, 0x2180);
  ASSERT_TRUE(Hint);
  const auto &Signature = Hint->Signature;
  EXPECT_EQ(Hint->ByteCount, 128U);
  ASSERT_EQ(Signature.ReturnType->Kind, NdTypeKind::Struct);
  EXPECT_EQ(Signature.ReturnType->Size, 128U);
  EXPECT_EQ(Signature.ReturnLocation.Kind,
            SourceABICarrierKind::IndirectResultPointer);
  EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, a64reg::X8);
  ASSERT_EQ(Signature.Parameters.size(), 4U);
  EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset, a64reg::X0);
  for (size_t J = 1; J != 4; ++J) {
    EXPECT_EQ(Signature.Parameters[J].Type->Kind, NdTypeKind::Float);
    EXPECT_EQ(Signature.Parameters[J].Location.RegisterOffset,
              getTargetRegInfo(Arch::AArch64).FPParamRegs[J - 1]);
  }
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    auto Changed = I;
    if (Mutation == 0)
      Changed.Arch = Arch::X64;
    else if (Mutation == 1)
      Changed.DyldBindSlots.at(0x2180).Module =
          "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
    else if (Mutation == 2)
      Changed.DyldBindSlots.at(0x2180).WeakImport = true;
    else if (Mutation == 3)
      Changed.DyldBindSlots.at(0x2180).Addend = 8;
    else if (Mutation == 4)
      Changed.IsRelocatable = true;
    else if (Mutation == 5)
      Changed.Bits = Bitness::Bits32;
    else
      Changed.ImportPtrSlots[0x2180] = "_CGAffineTransformScale";
    EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
  }
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Double = NdType::makeFloat(8);
  SourceFunctionTypeHint Entry;
  Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Entry.ReturnType = NdType::makeInt(8, false);
  Entry.Parameters = {{"input", Pointer},
                      {"output", Pointer},
                      {"sx", Double},
                      {"sy", Double},
                      {"sz", Double}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Arch::AArch64, Error));
  LowFunc F;
  F.Entry = 0x1200;
  F.Name = "scale_matrix";
  LowBlock B;
  B.Id = 0;
  B.StartAddr = F.Entry;
  B.EndAddr = 0x1214;
  B.Ops = {operation(NdOp::COPY, NdVar::reg(a64reg::X8, 8),
                     {NdVar::reg(a64reg::X1, 8)}, 0x1200),
           operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1204),
           operation(NdOp::COPY, NdVar::reg(a64reg::X8, 8), {NdVar::cst(0, 8)},
                     0x1208),
           operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8), {NdVar::cst(0, 8)},
                     0x120c),
           operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, 0x1210)};
  F.Blocks = {B};
  const std::map<va_t, SourceFunctionTypeHint> Entries{{F.Entry, Entry}};
  LowToMedConverter Converter;
  Converter.setBinaryImage(&I);
  Converter.setSourceCallHintsEnabled(true);
  Converter.setSourceCalleeTypeHints(&Entries);
  auto Med = Converter.convert(F, Arch::AArch64, BinaryFormat::MachO);
  Med.SourceTypeHint = Entry;
  recoverCallAbi(Med, Arch::AArch64, {}, &I);
  inferMedTypes(Med, Arch::AArch64);
  unsigned Stores = 0;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      Stores += Op.Opcode == NdOp::STORE && Op.Addr == 0x1204;
  EXPECT_EQ(Stores, 16U);
  auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  unsigned Calls = 0;
  walkStmts(High.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      ASSERT_FALSE(E && E->Kind == ExprKind::Undef);
      if (!E || E->Kind != ExprKind::Call)
        return;
      ++Calls;
      ASSERT_TRUE(E->SourceCallHint);
      EXPECT_EQ(E->Operands.size(), 4U);
      EXPECT_TRUE(sdk::objcSourceCallBound(*E, I, {}));
      for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
        auto Wrong = std::make_shared<SourceCallTypeHint>(*Hint);
        if (Mutation == 0)
          Wrong->ByteCount = 48;
        else if (Mutation == 1)
          Wrong->Signature.Parameters[0].Location.RegisterOffset = a64reg::X1;
        else if (Mutation == 2)
          Wrong->Signature.Parameters[3].Location.RegisterOffset =
              getTargetRegInfo(Arch::AArch64).FPParamRegs[3];
        else if (Mutation == 3)
          Wrong->Signature.ReturnLocation.RegisterOffset = a64reg::X0;
        else if (Mutation == 4)
          Wrong->WeakImport = true;
        else if (Mutation == 5)
          Wrong->ByteCount = 0;
        else
          Wrong->Signature.Parameters[3].Type = NdType::makeFloat(4);
        auto Changed = *E;
        Changed.SourceCallHint = Wrong;
        EXPECT_FALSE(sdk::objcSourceCallBound(Changed, I, {})) << Mutation;
      }
    });
  });
  ASSERT_EQ(Calls, 1U);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  ASSERT_NE(Source.find("memcpy(&value, transform, sizeof(value))"),
            std::string::npos)
      << Source;
  const auto Record = typeToC(Signature.ReturnType);
  Source += "\nstatic unsigned calls; static int mismatch;\n"
            "static uint64_t expected[16];\n" +
            Record + " scale_probe(" + Record +
            ", double, double, double) __asm__(\"_CATransform3DScale\");\n" +
            Record + " scale_probe(" + Record +
            " input, double sx, double sy, double sz) {\n"
            "  ++calls; uint64_t values[16], transformed[16];\n"
            "  memcpy(values, &input, sizeof(values));\n"
            "  mismatch |= memcmp(values, expected, sizeof(values)) != 0 || "
            "sx != 1.25 || sy != -2.5 || sz != 3.75;\n"
            "  for (unsigned j = 0; j != 16; ++j)\n"
            "    transformed[j] = values[(j + 7) % 16] ^ (0x100ULL + j);\n  " +
            Record +
            " result; memcpy(&result, transformed, sizeof(result)); "
            "return result;\n}\n"
            "int main(void) {\n"
            "  static const uint64_t cases[] = {0, 0x8000000000000000ULL, "
            "0x3ff0000000000000ULL, 0x7ff0000000000000ULL, "
            "0xfff0000000000000ULL, 0x7ff8000000001234ULL, 1, "
            "0x8000000000000001ULL};\n"
            "  for (unsigned i = 0; i != 512; ++i) {\n"
            "    for (unsigned j = 0; j != 16; ++j)\n"
            "      expected[j] = cases[(i + j) % 8] ^ "
            "((uint64_t)(i / 8) << 8) ^ j;\n"
            "    uint64_t input[18], output[18];\n"
            "    memset(input, 0xa5, sizeof(input));\n"
            "    memset(output, 0xa5, sizeof(output));\n"
            "    memcpy(input + 1, expected, sizeof(expected));\n"
            "    if (scale_matrix(input + 1, output + 1, 1.25, -2.5, 3.75)) "
            "return 1;\n"
            "    if (memcmp(input + 1, expected, sizeof(expected))) return 2;\n"
            "    if (scale_matrix(input + 1, input + 1, 1.25, -2.5, 3.75)) "
            "return 3;\n"
            "    for (unsigned j = 0; j != 16; ++j)\n"
            "      if (input[j + 1] != "
            "(expected[(j + 7) % 16] ^ (0x100ULL + j)) || "
            "output[j + 1] != input[j + 1]) return 4;\n"
            "    if (calls != (i + 1) * 2 || "
            "input[0] != 0xa5a5a5a5a5a5a5a5ULL || input[17] != input[0] || "
            "output[0] != input[0] || output[17] != input[0]) return 5;\n"
            "  }\n  return mismatch;\n}\n";
  executeSource(Source);
}

TEST(DarwinIndirectRecordCalls,
     RectangleTransformKeepsHFAAndIndirectInputIndependent) {
  constexpr auto CoreGraphics =
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
  auto I = image("_CGRectApplyAffineTransform");
  I.DynInfo.NeededLibs = {CoreGraphics};
  I.DyldBindSlots.at(0x2180).Module = CoreGraphics;
  const auto Hint = darwinRuntimeSourceCallHint(I, 0x2180);
  ASSERT_TRUE(Hint);
  const auto &Signature = Hint->Signature;
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  EXPECT_EQ(Hint->ByteCount, 48U);
  ASSERT_EQ(Signature.ReturnType->Kind, NdTypeKind::Struct);
  EXPECT_EQ(Signature.ReturnType->Size, 32U);
  ASSERT_EQ(Signature.ReturnComponents.size(), 4U);
  ASSERT_EQ(Signature.Parameters.size(), 2U);
  ASSERT_EQ(Signature.Parameters[0].Components.size(), 4U);
  EXPECT_EQ(Signature.Parameters[1].Type->Kind, NdTypeKind::Ptr);
  EXPECT_EQ(Signature.Parameters[1].Location.RegisterOffset, a64reg::X0);
  for (size_t J = 0; J != 4; ++J) {
    for (const auto &Carrier : {Signature.ReturnComponents[J],
                                Signature.Parameters[0].Components[J]}) {
      EXPECT_EQ(Carrier.Kind, SourceABICarrierKind::FloatingRegister);
      EXPECT_EQ(Carrier.RegisterOffset, TRI.FPParamRegs[J]);
      EXPECT_EQ(Carrier.ValueBytes, 8U);
    }
  }
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    auto Changed = I;
    if (Mutation == 0)
      Changed.Arch = Arch::X64;
    else if (Mutation == 1)
      Changed.DyldBindSlots.at(0x2180).Module = UIKit;
    else if (Mutation == 2)
      Changed.DyldBindSlots.at(0x2180).WeakImport = true;
    else if (Mutation == 3)
      Changed.DyldBindSlots.at(0x2180).Addend = 8;
    else if (Mutation == 4)
      Changed.IsRelocatable = true;
    else if (Mutation == 5)
      Changed.Bits = Bitness::Bits32;
    else if (Mutation == 6)
      Changed.ImportPtrSlots[0x2180] = "_CGRectApplyAffineTransform_extra";
    else
      Changed.ConflictingImportStorageSlots.insert(0x2180);
    EXPECT_FALSE(darwinRuntimeSourceCallHint(Changed, 0x2180)) << Mutation;
  }
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Double = NdType::makeFloat(8);
  SourceFunctionTypeHint Entry;
  Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Entry.ReturnType = NdType::makeInt(8, false);
  Entry.Parameters = {{"output", Pointer}, {"transform", Pointer},
                      {"x", Double},       {"y", Double},
                      {"width", Double},   {"height", Double}};
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Arch::AArch64, Error));
  LowFunc F;
  F.Entry = 0x1200;
  F.Name = "apply_rect";
  LowBlock B;
  B.Id = 0;
  B.StartAddr = F.Entry;
  B.Ops = {operation(NdOp::COPY, NdVar::reg(a64reg::X19, 8),
                     {NdVar::reg(a64reg::X0, 8)}, 0x1200),
           operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                     {NdVar::reg(a64reg::X1, 8)}, 0x1204),
           operation(NdOp::CALL, {}, {NdVar::cst(0x1100, 8)}, 0x1208)};
  va_t Address = 0x120c;
  for (size_t J = 0; J != 4; ++J) {
    B.Ops.push_back(
        operation(NdOp::INT_ADD, NdVar::reg(a64reg::X9, 8),
                  {NdVar::reg(a64reg::X19, 8), NdVar::cst(8 * J, 8)}, Address));
    Address += 4;
    B.Ops.push_back(operation(
        NdOp::STORE, {},
        {NdVar::reg(a64reg::X9, 8), NdVar::reg(TRI.FPParamRegs[J], 8)},
        Address));
    Address += 4;
  }
  B.Ops.push_back(operation(NdOp::COPY, NdVar::reg(a64reg::X0, 8),
                            {NdVar::cst(0, 8)}, Address));
  B.Ops.push_back(
      operation(NdOp::RETURN, {}, {NdVar::reg(a64reg::X0, 8)}, Address + 4));
  B.EndAddr = Address + 8;
  F.Blocks = {B};
  const std::map<va_t, SourceFunctionTypeHint> Entries{{F.Entry, Entry}};
  LowToMedConverter Converter;
  Converter.setBinaryImage(&I);
  Converter.setSourceCallHintsEnabled(true);
  Converter.setSourceCalleeTypeHints(&Entries);
  auto Med = Converter.convert(F, Arch::AArch64, BinaryFormat::MachO);
  Med.SourceTypeHint = Entry;
  recoverCallAbi(Med, Arch::AArch64, {}, &I);
  inferMedTypes(Med, Arch::AArch64);
  auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  unsigned Calls = 0;
  walkStmts(High.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      ASSERT_FALSE(E && E->Kind == ExprKind::Undef);
      if (!E || E->Kind != ExprKind::Call)
        return;
      ++Calls;
      ASSERT_TRUE(E->SourceCallHint);
      EXPECT_EQ(E->Operands.size(), 2U);
      EXPECT_TRUE(sdk::objcSourceCallBound(*E, I, {}));
      for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
        auto Wrong = std::make_shared<SourceCallTypeHint>(*Hint);
        if (Mutation == 0)
          Wrong->ByteCount = 32;
        else if (Mutation == 1)
          Wrong->Signature.Parameters[1].Location.RegisterOffset = a64reg::X1;
        else if (Mutation == 2)
          Wrong->Signature.Parameters[0].Components[3].Kind =
              SourceABICarrierKind::IntegerRegister;
        else if (Mutation == 3)
          Wrong->Signature.ReturnComponents[3].RegisterOffset =
              TRI.FPParamRegs[4];
        else if (Mutation == 4)
          Wrong->WeakImport = true;
        else if (Mutation == 5)
          Wrong->ByteCount = 0;
        else
          Wrong->Signature.ReturnLocation.Kind =
              SourceABICarrierKind::IndirectResultPointer;
        auto Changed = *E;
        Changed.SourceCallHint = Wrong;
        EXPECT_FALSE(sdk::objcSourceCallBound(Changed, I, {})) << Mutation;
      }
    });
  });
  ASSERT_EQ(Calls, 1U);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  ASSERT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  const auto Record = typeToC(Signature.ReturnType);
  Source +=
      "\nstatic unsigned calls; static int mismatch;\n"
      "static uint64_t rect_bits[4], transform_bits[6];\n" +
      Record + " rect_probe(" + Record +
      ", neverd_CGRectApplyAffineTransform_input) "
      "__asm__(\"_CGRectApplyAffineTransform\");\n" +
      Record + " rect_probe(" + Record +
      " rect, neverd_CGRectApplyAffineTransform_input transform) {\n"
      "  ++calls; uint64_t r[4], t[6], result_bits[4];\n"
      "  memcpy(r, &rect, sizeof(r)); memcpy(t, &transform, sizeof(t));\n"
      "  mismatch |= memcmp(r, rect_bits, sizeof(r)) != 0 || "
      "memcmp(t, transform_bits, sizeof(t)) != 0;\n"
      "  for (unsigned j = 0; j != 4; ++j)\n"
      "    result_bits[j] = r[(j + 1) % 4] ^ t[j] ^ t[4] ^ t[5];\n  " +
      Record +
      " result; memcpy(&result, result_bits, sizeof(result)); "
      "return result;\n}\n"
      "int main(void) {\n"
      "  static const uint64_t cases[] = {0, 0x8000000000000000ULL, "
      "0x3ff0000000000000ULL, 0x7ff0000000000000ULL, "
      "0xfff0000000000000ULL, 0x7ff8000000001234ULL, 1, "
      "0x8000000000000001ULL};\n"
      "  for (unsigned i = 0; i != 512; ++i) {\n"
      "    for (unsigned j = 0; j != 4; ++j)\n"
      "      rect_bits[j] = cases[(i + j) % 8] ^ ((uint64_t)(i / 8) << 8);\n"
      "    for (unsigned j = 0; j != 6; ++j)\n"
      "      transform_bits[j] = cases[(i + j + 3) % 8] ^ "
      "((uint64_t)(i / 8) << 16) ^ j;\n"
      "    double rect[4]; memcpy(rect, rect_bits, sizeof(rect));\n"
      "    uint64_t input[8], output[6];\n"
      "    memset(input, 0xa5, sizeof(input));\n"
      "    memset(output, 0xa5, sizeof(output));\n"
      "    memcpy(input + 1, transform_bits, sizeof(transform_bits));\n"
      "    if (apply_rect(output + 1, input + 1, "
      "rect[0], rect[1], rect[2], rect[3])) return 1;\n"
      "    if (memcmp(input + 1, transform_bits, sizeof(transform_bits))) "
      "return 2;\n"
      "    if (apply_rect(input + 1, input + 1, "
      "rect[0], rect[1], rect[2], rect[3])) return 3;\n"
      "    for (unsigned j = 0; j != 4; ++j)\n"
      "      if (input[j + 1] != (rect_bits[(j + 1) % 4] ^ "
      "transform_bits[j] ^ transform_bits[4] ^ transform_bits[5]) || "
      "output[j + 1] != input[j + 1]) return 4;\n"
      "    if (input[5] != transform_bits[4] || "
      "input[6] != transform_bits[5] || calls != (i + 1) * 2 || "
      "input[0] != 0xa5a5a5a5a5a5a5a5ULL || input[7] != input[0] || "
      "output[0] != input[0] || output[5] != input[0]) return 5;\n"
      "  }\n  return mismatch;\n}\n";
  executeSource(Source);
}
} // namespace

TEST(DarwinIndirectRecordCalls, MatrixFrameEffectsRequireExactCurrentContract) {
  constexpr auto QuartzCore =
      "/System/Library/Frameworks/QuartzCore.framework/QuartzCore";
  constexpr auto CoreGraphics =
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
  for (const char *Name :
       {"CATransform3DMakeTranslation", "CATransform3DScale",
        "CGAffineTransformMakeRotation", "CGAffineTransformMakeScale",
        "CGAffineTransformConcat", "CGAffineTransformInvert",
        "CGAffineTransformTranslate", "CGRectApplyAffineTransform"}) {
    SCOPED_TRACE(Name);
    const bool Rect = llvm::StringRef(Name) == "CGRectApplyAffineTransform";
    const bool Affine = llvm::StringRef(Name).starts_with("CGAffine") || Rect;
    const auto Provider = Affine ? CoreGraphics : QuartzCore;
    auto Image = image(("_" + std::string(Name)).c_str());
    Image.DynInfo.NeededLibs = {Provider};
    Image.DyldBindSlots.at(0x2180).Module = Provider;
    auto Binding = darwinRuntimeSourceCallHint(Image, 0x2180);
    ASSERT_TRUE(Binding);
    auto Effects = darwinMatrixSourceFrameEffects(Image, *Binding);
    ASSERT_TRUE(Effects);
    EXPECT_EQ(Effects->InitializesIndirectResult, !Rect);
    const unsigned Inputs =
        std::string(Name) == "CGAffineTransformConcat" ? 2U
        : std::string(Name) == "CATransform3DScale" || Rect ||
                std::string(Name) == "CGAffineTransformInvert" ||
                std::string(Name) == "CGAffineTransformTranslate"
            ? 1U
            : 0U;
    EXPECT_EQ(Binding->Signature.ReturnType->Size, Rect     ? 32U
                                                   : Affine ? 48U
                                                            : 128U);
    if (Rect)
      EXPECT_EQ(Binding->Signature.ReturnComponents.size(), 4U);
    else
      EXPECT_EQ(Binding->Signature.ReturnLocation.RegisterOffset, a64reg::X8);
    EXPECT_EQ(Effects->WritableFrameParameters.size(), Inputs);
    EXPECT_EQ(Effects->InitializedFrameParameters.size(),
              Effects->WritableFrameParameters.size());
    EXPECT_TRUE(Effects->ReadOnlyFrameParameters.empty());
    for (unsigned J = 0; J < Inputs; ++J) {
      const auto Parameter = Rect ? 1 : J;
      EXPECT_EQ(Effects->WritableFrameParameters.at(Parameter),
                Affine ? 48U : 128U);
      EXPECT_TRUE(Effects->InitializedFrameParameters.count(Parameter));
    }
    EXPECT_TRUE(Effects->ByValueFrameParameters.empty());
    EXPECT_FALSE(Effects->ReturnFrameOrExternal);
    EXPECT_FALSE(Effects->Scratch);
    for (unsigned Mutation = 0; Mutation < 22; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto I = Image;
      auto B = *Binding;
      switch (Mutation) {
      case 0:
        I.Arch = Arch::X64;
        break;
      case 1:
        I.Format = BinaryFormat::ELF;
        break;
      case 2:
        I.IsRelocatable = true;
        break;
      case 3:
        I.Bits = Bitness::Bits32;
        break;
      case 4:
        I.DyldBindSlots.at(0x2180).WeakImport = true;
        break;
      case 5:
        I.DyldBindSlots.at(0x2180).Addend = 8;
        break;
      case 6:
        I.DyldBindSlots.at(0x2180).Module = "/not/QuartzCore";
        break;
      case 7:
        I.DynInfo.NeededLibs.clear();
        break;
      case 8:
        I.ImportPtrSlots[0x2180] = "_other";
        break;
      case 9:
        I.ConflictingImportStorageSlots.insert(0x2180);
        break;
      case 10:
        B.WeakImport = true;
        break;
      case 11:
        B.DoesNotReturn = true;
        break;
      case 12:
        B.TargetName = "CATransform3DMakeScale";
        break;
      case 13:
        B.TargetAddress += 8;
        break;
      case 14:
        B.Signature.Parameters[0].Location.ValueBytes = 4;
        break;
      case 15:
        if (Rect)
          B.Signature.ReturnComponents[0].RegisterOffset = a64reg::X0;
        else
          B.Signature.ReturnLocation.RegisterOffset = a64reg::X0;
        break;
      case 16:
        B.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
        break;
      case 17:
        ++B.ByteCount;
        break;
      case 18:
        B.Format = SourceCallTypeHint::FormatArguments{};
        break;
      case 19:
        B.NilTerminated = SourceCallTypeHint::NilTerminatedArguments{};
        break;
      case 20:
        I.DyldBindSlots.at(0x2180).Module = Affine ? QuartzCore : CoreGraphics;
        I.DynInfo.NeededLibs = {I.DyldBindSlots.at(0x2180).Module};
        break;
      case 21:
        B.Signature.ReturnType =
            NdType::makeStruct({NdType::makePtr(NdType::makeVoid())});
        break;
      }
      EXPECT_FALSE(darwinMatrixSourceFrameEffects(I, B));
    }
  }
}

TEST(DarwinIndirectRecordCalls,
     MakeScaleProducesOnlyCompleteBoundedPrivateResultCopies) {
  constexpr auto CoreGraphics =
      "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics";
  auto Image = image("_CGAffineTransformMakeScale");
  Image.DynInfo.NeededLibs = {CoreGraphics};
  Image.DyldBindSlots.at(0x2180).Module = CoreGraphics;
  const auto Binding = darwinRuntimeSourceCallHint(Image, 0x2180);
  ASSERT_TRUE(Binding);
  const auto Effects = darwinMatrixSourceFrameEffects(Image, *Binding);
  ASSERT_TRUE(Effects);
  ASSERT_EQ(Binding->Signature.Parameters.size(), 2U);
  for (unsigned J = 0; J != 2; ++J) {
    const auto &Parameter = Binding->Signature.Parameters[J];
    EXPECT_EQ(Parameter.Type->Kind, NdTypeKind::Float);
    EXPECT_EQ(Parameter.Type->Size, 8U);
    EXPECT_EQ(Parameter.Location.Kind, SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Parameter.Location.RegisterOffset, a64reg::V0 + J * 16);
  }
  SourceFunctionTypeHint Entry, Consumer;
  Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Entry.ReturnType = NdType::makeVoid();
  Consumer = Entry;
  Consumer.Parameters = {{"transform", Binding->Signature.ReturnType}};
  std::string Error;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Entry, Arch::AArch64, Error));
  ASSERT_TRUE(assignDarwinFixedSourceABI(Consumer, Arch::AArch64, Error));
  ASSERT_TRUE(Consumer.Parameters[0].IndirectByValue);
  for (unsigned Offset : {0U, 4U, 8U, 16U, 24U, 64U}) {
    SCOPED_TRACE(Offset);
    LowFunc Low;
    Low.Entry = 0x1200;
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Low.Entry;
    Low.Blocks.push_back(Block);
    va_t Next = Low.Entry;
    const auto SP = NdVar::reg(a64reg::SP, 8);
    const auto LR = NdVar::reg(a64reg::X30, 8);
    const auto X0 = NdVar::reg(a64reg::X0, 8);
    const auto X8 = NdVar::reg(a64reg::X8, 8);
    const auto X9 = NdVar::reg(a64reg::X9, 8);
    const auto Add = [&](NdOp Opcode, NdVar Output,
                         std::initializer_list<NdVar> Inputs) {
      LowOp Op;
      Op.Opcode = Opcode;
      Op.Output = Output;
      Op.Addr = Next;
      Next += 4;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      Low.Blocks[0].Ops.push_back(Op);
    };
    Add(NdOp::INT_SUB, SP, {SP, NdVar::scalar(64, 8)});
    Add(NdOp::INT_ADD, X9, {SP, NdVar::scalar(56, 8)});
    Add(NdOp::STORE, {}, {X9, LR});
    Add(NdOp::INT_ADD, X8, {SP, NdVar::scalar(Offset, 8)});
    Add(NdOp::CALL, X0, {NdVar::cst(0x1100, 8)});
    const auto ProducerSite = nativeSourceCallKey(Low.Blocks[0].Ops.back());
    ASSERT_TRUE(ProducerSite);
    Add(NdOp::COPY, X0, {SP});
    Add(NdOp::CALL, X0, {NdVar::cst(0x1180, 8)});
    const auto ConsumerSite = nativeSourceCallKey(Low.Blocks[0].Ops.back());
    ASSERT_TRUE(ConsumerSite);
    Add(NdOp::INT_ADD, X9, {SP, NdVar::scalar(56, 8)});
    Add(NdOp::LOAD, LR, {X9});
    Add(NdOp::INT_ADD, SP, {SP, NdVar::scalar(64, 8)});
    Add(NdOp::RETURN, {}, {LR});
    NativeSourceCalls Calls;
    auto &ProducerCall = Calls[*ProducerSite];
    ProducerCall.Signature = &Binding->Signature;
    static_cast<SourceFrameEffects &>(ProducerCall) = *Effects;
    auto &ConsumerCall = Calls[*ConsumerSite];
    ConsumerCall.Signature = &Consumer;
    ConsumerCall.ByValueFrameParameters.insert(0);
    const auto Query = [&] {
      return sourceFrameByValueCopies(Low, Arch::AArch64, Calls, *ConsumerSite,
                                      Entry);
    };
    if (Offset) {
      EXPECT_FALSE(Query());
      continue;
    }
    ASSERT_TRUE(Query());
    EXPECT_EQ(*Query(), (std::vector<SourceFrameByValueCopy>{{0, -64, 48}}));
    EXPECT_TRUE(restoresNativeSourceState(Low, Arch::AArch64, Calls));
    // A physical return declaration alone cannot initialize private bytes.
    ProducerCall.InitializesIndirectResult = false;
    EXPECT_FALSE(Query());
  }
}
