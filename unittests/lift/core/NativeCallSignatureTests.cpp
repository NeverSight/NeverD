//===- NativeCallSignatureTests.cpp - Native scalar declarations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Complete native declarations retain arguments unused by the callee body.
///
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t Caller = 0x1000;
constexpr va_t Callee = 0x1100;

class NativeDeclaration final : public NullDebugContext {
public:
  FunctionSym Function;
  bool Authenticated = true;
  AuthenticatedReturnValueState ReturnState{AuthenticatedReturnKind::Integer,
                                            4};

  NativeDeclaration() {
    Function.Addr = Callee;
    Function.Name = "helper";
    Function.ReturnType = NdType::makeInt(4, false);
    Function.Params = {{"first", NdType::makeInt(4, false)},
                       {"unused", NdType::makeInt(4, false)}};
  }
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    return Address == Callee ? std::optional(Function) : std::nullopt;
  }
  bool hasInfo() const override { return true; }
  bool hasAuthenticatedFunctionSignatures() const override {
    return Authenticated;
  }
  AuthenticatedReturnValueState
  resolveAuthenticatedReturnValueState(va_t Address) const override {
    return Address == Function.Addr ? ReturnState
                                    : AuthenticatedReturnValueState{};
  }
};

BinaryImage integerImage(Arch Architecture, BinaryFormat Format) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Format = Format;
  Image.Bits = Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Image.Entry = Caller;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Caller;
  Text.Size = Text.FileSz = 0x120;
  Text.Data.resize(Text.Size);
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  const auto Bytes = [&](va_t Address, std::initializer_list<uint8_t> Code) {
    std::copy(Code.begin(), Code.end(), Text.Data.begin() + Address - Caller);
  };
  if (Architecture == Arch::AArch64) {
    // Save LR, pass 7 and 37, call a body that reads only its first input.
    const uint32_t Code[] = {0xa9bf7bfd, 0x910003fd, 0x528000e0, 0x528004a1,
                             0x9400003c, 0xa8c17bfd, 0xd65f03c0};
    for (unsigned I = 0; I < std::size(Code); ++I)
      llvm::support::endian::write32le(Text.Data.data() + 4 * I, Code[I]);
    llvm::support::endian::write32le(Text.Data.data() + 0x100, 0x11000c00);
    llvm::support::endian::write32le(Text.Data.data() + 0x104, 0xd65f03c0);
  } else if (Architecture == Arch::X86) {
    // push 37; push 7; call helper; add esp,8; ret
    Bytes(Caller,
          {0x6a, 37, 0x6a, 7, 0xe8, 0xf7, 0, 0, 0, 0x83, 0xc4, 8, 0xc3});
    Bytes(Callee, {0x8b, 0x44, 0x24, 4, 0x83, 0xc0, 3, 0xc3});
  } else {
    // Reserve the platform's call frame and set both argument registers.
    const bool Windows = Format == BinaryFormat::COFF;
    Bytes(Caller, {0x48,
                   0x83,
                   0xec,
                   uint8_t(Windows ? 40 : 8),
                   uint8_t(Windows ? 0xb9 : 0xbf),
                   7,
                   0,
                   0,
                   0,
                   uint8_t(Windows ? 0xba : 0xbe),
                   37,
                   0,
                   0,
                   0,
                   0xe8,
                   0xed,
                   0,
                   0,
                   0,
                   0x48,
                   0x83,
                   0xc4,
                   uint8_t(Windows ? 40 : 8),
                   0xc3});
    Bytes(Callee, {0x8d, uint8_t(Windows ? 0x41 : 0x47), 3, 0xc3});
  }
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back({"probe", Caller, 0x20, true});
  Image.Symbols.push_back({"helper", Callee, 8, true});
  return Image;
}

TEST(NativeCallSignatures, UnusedIntegerParametersReachBothSourceRoutes) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Exported : {false, true})
        for (bool LLVM : {false, true}) {
          SCOPED_TRACE(static_cast<unsigned>(Architecture));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(Exported);
          SCOPED_TRACE(LLVM);
          auto Image = integerImage(Architecture, Format);
          if (Exported) {
            Export Symbol;
            Symbol.Addr = Callee;
            Symbol.Name = "helper";
            Image.Exports.push_back(Symbol);
          }
          NativeDeclaration Debug;
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.OnlyFunctionEntries = {Caller};
          Options.EmitDumpOutput = false;
          Options.NoOpt = true;
          Options.LiftMode = LLVM;
          Options.SourceProjection = LLVM;
          auto Result = Pipeline().run(Image, Context, Options, &Debug);
          EXPECT_TRUE(Result.Success) << Result.Error;
          const auto Declaration =
              Result.CallEntryIntegerArguments.find(Callee);
          EXPECT_NE(Declaration, Result.CallEntryIntegerArguments.end());
          if (!Result.Success ||
              Declaration == Result.CallEntryIntegerArguments.end())
            continue;
          EXPECT_EQ(Declaration->second.Parameters.size(), 2u);
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            const auto *Helper = Result.LlvmModule->getFunction("helper");
            ASSERT_NE(Helper, nullptr);
            EXPECT_FALSE(Helper->isVarArg());
            EXPECT_EQ(Helper->arg_size(), 2u);
          }
          CEmitterOptions COptions;
          COptions.TheArch = Architecture;
          COptions.Format = Format;
          COptions.Image = &Image;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Out,
                                                 COptions, &Debug, &Image)
                           : HighCEmitter().emit(Result.HighFuncs, Out,
                                                 COptions, &Debug));
          // Observe both physical operands independently of the original
          // helper, whose body intentionally does not use the second one.
          Source += R"c(
static unsigned visits;
__typeof__(helper(0, 0)) helper(uint32_t first, uint32_t unused) {
  if (first != 7 || unused != 37) __builtin_trap();
  ++visits;
  return first + 3;
}
int main(void) { return probe() != 10 || visits != 1; }
)c";
          for (const auto Level : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(Source, {Level});
        }
}

TEST(NativeCallSignatures, IncompleteOrNonPlatformDeclarationsStayUnbound) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (bool Floating : {false, true})
      for (unsigned Variant = 0; Variant < 11; ++Variant) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(Floating);
        SCOPED_TRACE(Variant);
        auto Image = integerImage(Architecture, BinaryFormat::ELF);
        NativeDeclaration Debug;
        if (Floating) {
          Debug.Function.ReturnType = NdType::makeFloat(4);
          Debug.Function.Params[1].second = NdType::makeFloat(4);
          Debug.ReturnState = {AuthenticatedReturnKind::FloatingPoint, 4};
        }
        switch (Variant) {
        case 0:
          Debug.Authenticated = false;
          break;
        case 1:
          Debug.ReturnState = {};
          break;
        case 2:
          --Debug.Function.Addr;
          break;
        case 3:
          Debug.Function.IsVariadic = true;
          break;
        case 4:
          Debug.Function.PlatformConvention = false;
          break;
        case 5:
          Debug.Function.CallConv = DebugCallConv::Fastcall;
          break;
        case 6:
          Debug.Function.CallConv = DebugCallConv::Thiscall;
          break;
        case 7:
          Debug.Function.ParamEntryLocations.emplace_back();
          break;
        case 8:
          Debug.Function.Params[0].second = {};
          break;
        case 9:
          Debug.Function.ReturnType = {};
          break;
        case 10:
          Debug.Function.Params[0].second = NdType::makeInt(16);
          break;
        }
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.OnlyFunctionEntries = {Caller};
        Options.EmitDumpOutput = false;
        Options.NoOpt = true;
        auto Result = Pipeline().run(Image, Context, Options, &Debug);
        EXPECT_FALSE(Result.CallEntryIntegerArguments.count(Callee));
        EXPECT_FALSE(Result.CallEntryFloatingArguments.count(Callee));
        EXPECT_FALSE(Result.CallPrototypeArguments.count(Callee));
      }
}

TEST(NativeCallSignatures, MixedScalarArgumentsKeepTheirPhysicalCarriers) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(LLVM);
        auto Image = integerImage(Architecture, Format);
        auto &Bytes = Image.Segments.front().Data;
        if (Architecture == Arch::AArch64) {
          const uint32_t Code[] = {
              0xa9bf7bfd, 0x910003fd, 0x528000e0, 0x52a80401, // w1=2.5f bits
              0x1e270020, 0x9400003b, 0xa8c17bfd, 0xd65f03c0};
          for (unsigned I = 0; I < std::size(Code); ++I)
            llvm::support::endian::write32le(Bytes.data() + 4 * I, Code[I]);
        } else if (Architecture == Arch::X86) {
          const uint8_t Code[] = {
              0x68, 0,    0, 0x20, 0x40, 0x6a, 7, // push 2.5f; push 7
              0xe8, 0xf4, 0, 0,    0,    0x83, 0xc4, 8, 0xc3};
          std::copy(std::begin(Code), std::end(Code), Bytes.begin());
        } else {
          const bool Windows = Format == BinaryFormat::COFF;
          const uint8_t Code[] = {0x48,
                                  0x83,
                                  0xec,
                                  uint8_t(Windows ? 40 : 8),
                                  uint8_t(Windows ? 0xb9 : 0xbf),
                                  7,
                                  0,
                                  0,
                                  0,
                                  0xb8,
                                  0,
                                  0,
                                  0x20,
                                  0x40, // eax=2.5f bits
                                  0x66,
                                  0x0f,
                                  0x6e,
                                  uint8_t(Windows ? 0xc8 : 0xc0),
                                  0xe8,
                                  0xe9,
                                  0,
                                  0,
                                  0,
                                  0x48,
                                  0x83,
                                  0xc4,
                                  uint8_t(Windows ? 40 : 8),
                                  0xc3};
          std::copy(std::begin(Code), std::end(Code), Bytes.begin());
        }
        NativeDeclaration Debug;
        Debug.Function.Params[1].second = NdType::makeFloat(4);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.OnlyFunctionEntries = {Caller};
        Options.EmitDumpOutput = false;
        Options.NoOpt = true;
        Options.LiftMode = Options.SourceProjection = LLVM;
        auto Result = Pipeline().run(Image, Context, Options, &Debug);
        ASSERT_TRUE(Result.Success) << Result.Error;
        const auto Signature = Result.CallEntryFloatingArguments.find(Callee);
        EXPECT_NE(Signature, Result.CallEntryFloatingArguments.end());
        if (Signature == Result.CallEntryFloatingArguments.end())
          continue;
        EXPECT_EQ(Signature->second.Scalars.size(), 2u);
        if (LLVM)
          EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        CEmitterOptions COptions;
        COptions.TheArch = Architecture;
        COptions.Format = Format;
        COptions.Image = &Image;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Out,
                                               COptions, &Debug, &Image)
                         : HighCEmitter().emit(Result.HighFuncs, Out, COptions,
                                               &Debug));
        Source += R"c(
static unsigned visits;
uint32_t helper(uint32_t first, float unused) {
  if (first != 7 || unused != 2.5f) __builtin_trap();
  ++visits;
  return first + 3;
}
int main(void) { return probe() != 10 || visits != 1; }
)c";
        for (const auto Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(Source, {Level});
      }
}

TEST(NativeCallSignatures,
     OmittedForwardersRetainDeclarationsAcrossSelections) {
  constexpr va_t Forwarder = 0x1080;
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(LLVM);
        auto Image = integerImage(Architecture, Format);
        auto &Bytes = Image.Segments.front().Data;
        if (Architecture == Arch::AArch64) {
          llvm::support::endian::write32le(Bytes.data() + 16, 0x9400001c);
          llvm::support::endian::write32le(Bytes.data() + 0x80, 0x14000020);
        } else {
          const unsigned Displacement = Architecture == Arch::X86 ? 5 : 15;
          Bytes[Displacement] -= 0x80;
          Bytes[0x80] = 0xe9;
          llvm::support::endian::write32le(Bytes.data() + 0x81, 0x7b);
        }
        Image.Symbols.push_back({"forwarder", Forwarder,
                                 Architecture == Arch::AArch64 ? 4u : 5u,
                                 true});
        NativeDeclaration Debug;
        PipelineAnalysisCache Cache;
        for (bool Cached : {false, true})
          for (const auto &Entries :
               {std::set<va_t>{Caller}, std::set<va_t>{Caller, Forwarder},
                std::set<va_t>{Caller}}) {
            SCOPED_TRACE(Cached);
            SCOPED_TRACE(Entries.size());
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.OnlyFunctionEntries = Entries;
            Options.EmitDumpOutput = false;
            Options.NoOpt = true;
            Options.LiftMode = Options.SourceProjection = LLVM;
            Options.AnalysisCache = Cached ? &Cache : nullptr;
            const auto Result = Pipeline().run(Image, Context, Options, &Debug);
            ASSERT_TRUE(Result.Success) << Result.Error;
            unsigned Calls = 0;
            if (LLVM) {
              EXPECT_FALSE(
                  llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
              const auto *Function = Result.LlvmModule->getFunction("probe");
              ASSERT_NE(Function, nullptr);
              for (const auto &Block : *Function)
                for (const auto &Instruction : Block)
                  if (const auto *Call =
                          llvm::dyn_cast<llvm::CallBase>(&Instruction);
                      Call && Call->getCalledFunction()) {
                    auto Address = rewrite_source::getOriginalVA(
                        *Call->getCalledFunction());
                    ASSERT_TRUE(static_cast<bool>(Address))
                        << llvm::toString(Address.takeError());
                    if (!*Address)
                      continue;
                    EXPECT_TRUE(**Address == Forwarder || **Address == Callee);
                    ++Calls;
                    EXPECT_EQ(Call->arg_size(), 2u);
                  }
            } else {
              const auto Function =
                  llvm::find_if(Result.HighFuncs, [](const auto &F) {
                    return F.Entry == Caller;
                  });
              ASSERT_NE(Function, Result.HighFuncs.end());
              const auto Visit = [&](const auto &Self,
                                     const ExprPtr &E) -> void {
                if (E->Kind == ExprKind::Call &&
                    E->IntrinsicId == Intrinsic::None) {
                  ++Calls;
                  EXPECT_EQ(E->Operands.size(), 2u);
                }
                E->forEachChildExpr(
                    [&](const ExprPtr &Child) { Self(Self, Child); });
              };
              walkStmts(Function->Body, [&](const HighStmt &Statement) {
                forEachExpr(Statement, [&](const ExprPtr &Expression) {
                  Visit(Visit, Expression);
                });
              });
            }
            EXPECT_EQ(Calls, 1u);
          }
      }
}
} // namespace
