//===- SourceCallIntegerViewTests.cpp - Integer views of call results -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute explicit narrow-to-wide result conversions after a typed import.
/// The caller's instructions, rather than the import ABI's unused bits, fix
/// the complete return word.
///
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Entry = 0x10000;
constexpr va_t ImportSlot = 0x20000;

class IntegerHeader : public NullDebugContext {
public:
  bool Signed = true;

  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    if (Name != "integer_provider")
      return std::nullopt;
    FunctionSym Function;
    Function.Name = Name.str();
    Function.PrototypeFromHeader = true;
    Function.PrototypeLibrary = "test-integer-results";
    Function.CallConv = DebugCallConv::Cdecl;
    Function.ReturnType = NdType::makeInt(4, Signed);
    Function.Params = {{"bits", NdType::makeInt(4, false)}};
    Function.PortablePrototypeDeclaration =
        std::string(Signed ? "int32_t" : "uint32_t") +
        " integer_provider(uint32_t bits)";
    return Function;
  }

  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    auto Function = Address == ImportSlot ? resolvePrototype("integer_provider")
                                          : std::nullopt;
    if (Function)
      Function->Addr = Address;
    return Function;
  }
};

BinaryImage fixture(Arch Architecture, BinaryFormat Format, unsigned Bits,
                    bool SignExtend) {
  std::vector<uint8_t> Code;
  if (Architecture == Arch::X64) {
    const uint8_t Frame = Format == BinaryFormat::COFF ? 40 : 8;
    Code = {0x48, 0x83, 0xEC, Frame, 0xFF, 0x15}; // sub rsp; call [rip+disp]
    const uint32_t Displacement = ImportSlot - (Entry + 10);
    for (unsigned Byte = 0; Byte != 4; ++Byte)
      Code.push_back(Displacement >> (Byte * 8));
    if (!Bits) {
      // Observe the lifted call carrier without an explicit conversion.
    } else if (SignExtend) {
      if (Bits == 32)
        Code.insert(Code.end(), {0x48, 0x63, 0xC0}); // movsxd rax,eax
      else
        Code.insert(Code.end(), {0x48, 0x0F, uint8_t(Bits == 8 ? 0xBE : 0xBF),
                                 0xC0}); // movsx rax,al/ax
    } else if (Bits == 32)
      Code.insert(Code.end(), {0x89, 0xC0}); // mov eax,eax
    else
      Code.insert(Code.end(), {0x0F, uint8_t(Bits == 8 ? 0xB6 : 0xB7),
                               0xC0}); // movzx eax,al/ax
    // Observe the complete RAX before returning: a narrow ordinary return
    // contract cannot hide a changed upper word.
    Code.insert(Code.end(), {0x49, 0xBB, 0, 0, 0, 0, 1, 0, 0, 0, 0x4C, 0x31,
                             0xD8}); // mov r11,1<<32; xor rax,r11
    Code.insert(Code.end(), {0x48, 0x83, 0xC4, Frame, 0xC3}); // add rsp; ret
  } else {
    const uint32_t Conversion =
        !Bits        ? 0u
        : SignExtend ? 0x93400000u | ((Bits - 1) << 10) // sxtb/sxth/sxtw x0,w0
        : Bits == 32 ? 0x2A0003E0u                      // mov w0,w0
                     : 0x53000000u | ((Bits - 1) << 10);
    const uint32_t Words[] = {
        0xA9BF7BFD, // stp x29,x30,[sp,-16]!
        0x910003FD, // mov x29,sp
        0x90000090, // adrp x16,0x20000 (from 0x10008)
        0xF9400210, // ldr x16,[x16]
        0xD63F0200, // blr x16
        Conversion,
        0xD2C00031, // mov x17,1<<32
        0xCA110000, // eor x0,x0,x17
        0xA8C17BFD, // ldp x29,x30,[sp],16
        0xD65F03C0, // ret
    };
    for (uint32_t Word : Words)
      if (Word)
        for (unsigned Byte = 0; Byte != 4; ++Byte)
          Code.push_back(Word >> (Byte * 8));
  }
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Format = Format;
  Image.Bits = Bitness::Bits64;
  Image.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = Text.FileSz = Code.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Code);
  Image.Segments.push_back(std::move(Text));
  Segment Iat;
  Iat.Name = ".idata";
  Iat.VA = ImportSlot;
  Iat.Size = Iat.FileSz = 8;
  Iat.Flags = SegmentFlags::Readable;
  Iat.Data.resize(8);
  Image.Segments.push_back(std::move(Iat));
  Import Provider;
  Provider.Name = "integer_provider";
  Provider.Module = "fixture.dll";
  Provider.IATAddr = ImportSlot;
  Image.Imports.push_back(std::move(Provider));
  EXPECT_TRUE(Image.recordImportStorageSlot(ImportSlot, "integer_provider", 0,
                                            ImportStorageEvidence::LoaderBind));
  auto Function = Symbol::makeFunc(Entry);
  Function.Name = "integer_view";
  Image.Symbols.push_back(std::move(Function));
  return Image;
}

/// On the matching Linux x64 host, run the original independently encoded
/// caller with a native provider. This checks the full physical return word,
/// including Win64's register arguments and caller-owned home area. Other
/// hosts still execute the generated C, without claiming this native control.
std::string nativeOracle(const BinaryImage &Image) {
  if (Image.Arch != Arch::X64)
    return {};
  const bool Windows = Image.Format == BinaryFormat::COFF;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  Out << "\n#if defined(__linux__) && defined(__x86_64__)\n"
         "#include <sys/mman.h>\n#include <string.h>\n"
         "static "
      << (Windows ? "__attribute__((ms_abi)) " : "")
      << "int32_t native_provider(uint32_t bits) {\n"
         "  ++provider_calls; return (int32_t)bits;\n}\n"
         "static uint64_t native_integer_view(uint32_t bits) {\n"
         "  static const unsigned char original[] = {";
  for (uint8_t Byte : Image.Segments.front().Data)
    Out << unsigned(Byte) << ',';
  Out << "};\n"
         "  const size_t size = "
      << ImportSlot - Entry + 8
      << ";\n"
         "  unsigned char *mapping = mmap(0, size + 16, "
         "PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, "
         "0);\n"
         "  if (mapping == MAP_FAILED) __builtin_trap();\n"
         // Keep the UBSan function-signature probe's prefix accessible.
         "  unsigned char *code = mapping + 16;\n"
         "  memcpy(code, original, sizeof(original));\n"
         "  uintptr_t target = (uintptr_t)native_provider;\n"
         "  memcpy(code + "
      << ImportSlot - Entry
      << ", &target, 8);\n"
         "  __builtin___clear_cache((char *)code, (char *)code + size);\n"
         "  uint64_t result = ((uint64_t ("
      << (Windows ? "__attribute__((ms_abi)) " : "")
      << "*)(uint32_t))code)(bits);\n"
         "  if (munmap(mapping, size + 16)) __builtin_trap();\n"
         "  return result;\n}\n#endif\n";
  return Source;
}

class SourceCallIntegerViews
    : public ::testing::TestWithParam<
          std::tuple<Arch, BinaryFormat, unsigned, bool, bool>> {};

TEST_P(SourceCallIntegerViews, ExplicitConversionsKeepEveryReturnedBit) {
  const auto [Architecture, Format, Bits, SignExtend, Signed] = GetParam();
  const unsigned ObservedBits = Bits ? Bits : 32;
  SCOPED_TRACE(unsigned(Architecture));
  SCOPED_TRACE(unsigned(Format));
  SCOPED_TRACE(Bits);
  SCOPED_TRACE(SignExtend);
  SCOPED_TRACE(Signed);
  const auto Image = fixture(Architecture, Format, Bits, SignExtend);
  IntegerHeader Header;
  Header.Signed = Signed;
  for (bool LLVM : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(NoOpt);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Entry};
      Options.SourceProjection = true;
      Options.EmitDumpOutput = false;
      Options.LiftMode = LLVM;
      Options.NoOpt = NoOpt;
      auto Result = Pipeline().run(Image, Context, Options, &Header);
      ASSERT_TRUE(Result.Success) << Result.Error;
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      CEmitterOptions COptions;
      COptions.TheArch = Architecture;
      COptions.Format = Format;
      COptions.Image = &Image;
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions,
                                        &Header, &Image));
      } else
        ASSERT_TRUE(
            HighCEmitter().emit(Result.HighFuncs, Output, COptions, &Header));
      ASSERT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
      const std::string Oracle =
          Header.resolvePrototype("integer_provider")
              ->PortablePrototypeDeclaration +
          R"( {
  ++provider_calls;
  return bits;
}
int main(void) {
  static const uint32_t inputs[] = {0, 1, 0x7f, 0x80, 0xff, 0x100,
    0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff};
  for (unsigned i = 0; i != sizeof(inputs) / sizeof(inputs[0]); ++i) {
    uint64_t expected = inputs[i] & )" +
          std::to_string((uint64_t{1} << ObservedBits) - 1) + "ULL;\n" +
          (SignExtend ? "    if (expected & (1ULL << " +
                            std::to_string(Bits - 1) + ")) expected |= ~" +
                            std::to_string((uint64_t{1} << Bits) - 1) + "ULL;\n"
                      : "") +
          "    expected ^= 1ULL << 32;\n" +
          (Architecture == Arch::X64 ? R"(
#if defined(__linux__) && defined(__x86_64__)
    provider_calls = 0;
    if (native_integer_view(inputs[i]) != expected || provider_calls != 1)
      return 2;
#endif
)"
                                     : "") +
          R"(    provider_calls = 0;
    if ((uint64_t)integer_view(inputs[i]) != expected || provider_calls != 1)
      return 1;
  }
  return 0;
}
)";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            "static unsigned provider_calls;\n" + Source + nativeOracle(Image) +
                Oracle,
            {Level, "-D_GNU_SOURCE", "-fsanitize=undefined",
             "-fsanitize-trap=all"});
    }
}

INSTANTIATE_TEST_SUITE_P(
    NativeFormats, SourceCallIntegerViews, ::testing::ValuesIn([] {
      std::vector<SourceCallIntegerViews::ParamType> Cases;
      for (auto Architecture : {Arch::X64, Arch::AArch64})
        for (auto Format :
             {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
          for (unsigned Bits : {8u, 16u, 32u, 0u})
            for (bool SignExtend : {false, true})
              for (bool Signed : {false, true})
                if (Bits || !SignExtend)
                  Cases.emplace_back(Architecture, Format, Bits, SignExtend,
                                     Signed);
      return Cases;
    }()));

TEST(SourceCallIntegerRendering, NarrowConditionsAndReturnsKeepTheirOwnBits) {
  IntegerHeader Header;
  Header.Signed = false;
  const auto Integer = NdType::makeInt(4, false);
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (unsigned Bytes : {1u, 2u})
        for (bool Slice : {false, true})
          for (unsigned Use : {0u, 1u, 2u}) {
            SCOPED_TRACE(Bytes);
            SCOPED_TRACE(Slice);
            SCOPED_TRACE(Use);
            SCOPED_TRACE(unsigned(Architecture));
            SCOPED_TRACE(unsigned(Format));
            MedVar Parameter;
            Parameter.Kind = MedVar::Param;
            Parameter.Id = 0;
            Parameter.Size = 4;
            Parameter.TheArch = Architecture;
            auto Call =
                HighExpr::makeCall("integer_provider", ImportSlot,
                                   {HighExpr::makeVar(Parameter, Integer)});
            Call->Type = Integer;
            auto View = Slice ? HighExpr::makeBinop(NdOp::SUBBYTES, Call,
                                                    HighExpr::makeConst(0, 4))
                              : std::make_shared<HighExpr>();
            View->Type = NdType::makeInt(Bytes, false);
            if (!Slice) {
              View->Kind = ExprKind::Cast;
              View->CastTo = View->Type;
              View->Operands.push_back(Call);
            }
            HighStmt Yes, No;
            Yes.Kind = No.Kind = StmtKind::Return;
            Yes.RetVal = HighExpr::makeConst(1, 4);
            No.RetVal = HighExpr::makeConst(0, 4);
            HighStmt If;
            If.Kind = StmtKind::IfElse;
            If.Cond = Use == 1
                          ? HighExpr::makeBinop(NdOp::INT_EQUAL, View,
                                                HighExpr::makeConst(0, Bytes))
                          : View;
            If.Body = {Yes};
            If.ElseBody = {No};
            HighFunc Function;
            Function.Name = "narrow_condition";
            Function.Entry = Entry;
            Function.ReturnType = NdType::makeInt(8, false);
            Function.Params = {{"arg0", Integer}};
            Function.Body = {If};
            if (Use == 2) {
              Function.Body = {Yes};
              Function.Body.front().RetVal = View;
            }
            std::string Source;
            llvm::raw_string_ostream Output(Source);
            CEmitterOptions Options;
            Options.TheArch = Architecture;
            Options.Format = Format;
            ASSERT_TRUE(
                HighCEmitter().emit({Function}, Output, Options, &Header));
            const std::string Oracle = R"(
uint32_t integer_provider(uint32_t bits) { ++provider_calls; return bits; }
int main(void) {
  static const uint32_t inputs[] = {0, 1, 0x100, 0x101, 0x10000, 0x10001};
  for (unsigned i = 0; i != sizeof(inputs) / sizeof(inputs[0]); ++i) {
    unsigned expected = (inputs[i] & )" +
                                       std::to_string((1u << (Bytes * 8)) - 1) +
                                       (Use == 1   ? "u) == 0;\n"
                                        : Use == 2 ? "u);\n"
                                                   : "u) != 0;\n") +
                                       R"(    provider_calls = 0;
    if (narrow_condition(inputs[i]) != expected || provider_calls != 1)
      return 1;
  }
  return 0;
}
)";
            for (llvm::StringRef Level : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(
                  "static unsigned provider_calls;\n" + Source + Oracle,
                  {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
          }
}

TEST(SourceCallIntegerLowering, DirectAndLoadedCallsKeepTheirLiftedCarrier) {
  IntegerHeader Header;
  const std::map<va_t, std::string> Names{{ImportSlot, "integer_provider"}};
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (auto Opcode : {NdOp::CALL, NdOp::INDIR_CALL}) {
        SCOPED_TRACE(unsigned(Architecture));
        SCOPED_TRACE(unsigned(Format));
        SCOPED_TRACE(unsigned(Opcode));
        MedVar Result;
        Result.Kind = MedVar::Reg;
        Result.Id = 1;
        Result.SSAVer = 1;
        Result.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
        Result.Size = 8;
        Result.TheArch = Architecture;
        MedOp Call;
        Call.Opcode = Opcode;
        Call.Addr = Entry;
        Call.Output = Result;
        Call.addInput(MedVar::makeConst(ImportSlot, 8));
        Call.addInput(MedVar::makeConst(UINT32_MAX, 4));
        MedOp Return;
        Return.Opcode = NdOp::RETURN;
        Return.Addr = Entry + 8;
        Return.addInput(Result);
        MedFunc Function;
        Function.Entry = Entry;
        Function.Name = "lowered_carrier";
        Function.ReturnType = NdType::makeInt(8, false);
        MedBlock Block;
        Block.Id = 0;
        Block.StartAddr = Entry;
        Block.Ops = {Call, Return};
        Function.Blocks.push_back(std::move(Block));
        MedCallInfo Arguments;
        Arguments.BlockId = 0;
        Arguments.OpIdx = 0;
        Arguments.TargetAddr = ImportSlot;
        Arguments.TargetName = "integer_provider";
        Arguments.Args = {Call.Inputs[1]};
        Function.CallInfos.push_back(std::move(Arguments));
        MedToHighConverter Converter;
        Converter.setFuncNames(&Names);
        auto High = Converter.convert(Function, Architecture);
        bool Found = false;
        walkStmts(High.Body, [&](const HighStmt &Statement) {
          forEachRhsExpr(Statement, [&](const ExprPtr &Value) {
            if (!Value || Value->Kind != ExprKind::Call)
              return;
            Found = true;
            EXPECT_TRUE(Value->Type);
            if (Value->Type)
              EXPECT_EQ(Value->Type->Size, Result.Size);
          });
        });
        EXPECT_TRUE(Found);
        std::string Source;
        llvm::raw_string_ostream Output(Source);
        CEmitterOptions Options;
        Options.TheArch = Architecture;
        Options.Format = Format;
        ASSERT_TRUE(HighCEmitter().emit({High}, Output, Options, &Header));
        const std::string Oracle = R"(
int32_t integer_provider(uint32_t bits) { ++provider_calls; return bits; }
int main(void) {
  return lowered_carrier() != UINT64_C(0xffffffff) || provider_calls != 1;
}
)";
        for (llvm::StringRef Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              "static unsigned provider_calls;\n" + Source + Oracle,
              {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
      }
}
} // namespace
