//===- X64DispatchFrameSourceTests.cpp - Physical bridge source
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks exact recognition and native execution of physical dispatch bridges.
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/loader/X86/X64DispatchFrame.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

using namespace neverd;
namespace {
constexpr va_t Entry = 0x1100, Linkage = 0x1000, Dispatcher = 0x1200,
               Caller = 0x1280;

BinaryImage bridgeImage(uint32_t Selector = 0xf1234567, int8_t Segment = 16) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Entry = Entry;
  Image.Base = Linkage;
  neverd::Segment Code;
  Code.Name = ".text";
  Code.VA = Linkage;
  Code.Size = Code.FileSz = 0x300;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data.resize(Code.Size, 0xcc);
  const uint8_t Bytes[] = {0x48, 0x8b, 0xc4, 0xfa, 0x48, 0x83, 0xec, 0x10,
                           0x50, 0x9c, 0x6a, 0,    0x48, 0x8d, 0x05, 0,
                           0,    0,    0,    0x50, 0xb8, 0,    0,    0,
                           0,    0xe9, 0,    0,    0,    0};
  auto *Body = Code.Data.data() + Entry - Linkage;
  std::copy(std::begin(Bytes), std::end(Bytes), Body);
  Body[11] = Segment;
  llvm::support::endian::write32le(
      Body + 15, static_cast<uint32_t>(Linkage - (Entry + 19)));
  llvm::support::endian::write32le(Body + 21, Selector);
  llvm::support::endian::write32le(Body + 26, Dispatcher - (Entry + 30));
  Code.Data[0] = 0xc3;
  const uint8_t Return[] = {0x48, 0x8b, 0x64, 0x24, 0x18, 0xc3};
  std::copy(std::begin(Return), std::end(Return),
            Code.Data.data() + Dispatcher - Linkage);
  Image.Segments.push_back(std::move(Code));
  for (auto [Address, Name] :
       {std::pair{Entry, "bridge_entry"}, std::pair{Linkage, "native_linkage"},
        std::pair{Dispatcher, "native_dispatch"}}) {
    auto Symbol = Symbol::makeFunc(Address);
    Symbol.Name = Name;
    Image.Symbols.push_back(std::move(Symbol));
  }
  return Image;
}

TEST(X64DispatchFrame, RequiresEveryOriginalByteAndCodeTarget) {
  auto Image = bridgeImage();
  auto Frame = x64DispatchFrame(Image, Entry);
  ASSERT_TRUE(Frame);
  EXPECT_EQ(Frame->Selector, 0xf1234567u);
  EXPECT_EQ(Frame->Segment, 16);
  EXPECT_EQ(Frame->Linkage, Linkage);
  EXPECT_EQ(Frame->Target, Dispatcher);
  EXPECT_EQ(Frame->LinkageSymbol, "native_linkage");
  EXPECT_EQ(Frame->TargetSymbol, "native_dispatch");
  for (size_t Offset : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 12u, 13u,
                        14u, 19u, 20u, 25u}) {
    auto Changed = bridgeImage();
    Changed.Segments[0].Data[Entry - Linkage + Offset] ^= 1;
    EXPECT_FALSE(x64DispatchFrame(Changed, Entry)) << Offset;
  }
  for (size_t Offset : {15u, 26u}) {
    auto Changed = bridgeImage();
    llvm::support::endian::write32le(Changed.Segments[0].Data.data() + Entry -
                                         Linkage + Offset,
                                     -static_cast<int32_t>(Offset + 4));
    EXPECT_FALSE(x64DispatchFrame(Changed, Entry));
    llvm::support::endian::write32le(
        Changed.Segments[0].Data.data() + Entry - Linkage + Offset, 0x7fffffff);
    EXPECT_FALSE(x64DispatchFrame(Changed, Entry));
  }
  Image.Segments[0].Data.resize(Entry - Linkage + 29);
  EXPECT_FALSE(x64DispatchFrame(Image, Entry));
  EXPECT_FALSE(x64DispatchFrame(Image, InvalidVA - 10));
}

TEST(X64DispatchFrame, RejectsForeignImagesRelocationsAndAssemblyNames) {
  for (unsigned Case = 0; Case < 9; ++Case) {
    auto Image = bridgeImage();
    switch (Case) {
    case 0:
      Image.Arch = Arch::X86;
      break;
    case 1:
      Image.Bits = Bitness::Bits32;
      break;
    case 2:
      Image.Format = BinaryFormat::ELF;
      break;
    case 3:
      Image.IsRelocatable = true;
      break;
    case 4:
      Image.BaseRelocations.push_back({Entry + 15, 10});
      break;
    case 5:
      Image.CodePtrRelocSlots.insert(Entry + 21);
      break;
    case 6:
      Image.Symbols[1].Name = "bad; ret";
      break;
    case 8:
      Image.BaseRelocations.push_back({Entry - 7, 10});
      break;
    case 7:
      Image.Segments[0].Flags = SegmentFlags::Readable;
      break;
    }
    EXPECT_FALSE(x64DispatchFrame(Image, Entry)) << Case;
  }
  EXPECT_EQ(x64DispatchFrame(bridgeImage(0, -128), Entry)->Segment, -128);
}

class X64DispatchFrameSource : public NeverDLiftTest {};

TEST_F(X64DispatchFrameSource, BothRoutesRetainThePhysicalMachineInterface) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang";
  class NativeHeader : public NullDebugContext {
  public:
    bool hasInfo() const override { return true; }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address == Caller) {
        FunctionSym Function;
        Function.Addr = Caller;
        Function.Name = "bridge_caller";
        Function.ReturnType = NdType::makeInt(4, true);
        return Function;
      }
      if (Address != Entry)
        return std::nullopt;
      FunctionSym Function;
      Function.Addr = Entry;
      Function.Name = "ZwQueryVirtualMemory";
      Function.ReturnType = NdType::makeInt(4, true);
      Function.PrototypeLibrary = "synthetic-native-header";
      Function.PrototypeFromHeader = true;
      Function.Params = {{"a", NdType::makeInt(8, false)},
                         {"b", NdType::makePtr(NdType::makeVoid())},
                         {"c", NdType::makeInt(4, false)},
                         {"d", NdType::makePtr(NdType::makeVoid())},
                         {"e", NdType::makeInt(8, false)},
                         {"f", NdType::makePtr(NdType::makeVoid())}};
      return Function;
    }
  } Header;
  for (bool Typed : {false, true}) {
    SCOPED_TRACE(Typed);
    DebugContext *Debug = Typed ? &Header : nullptr;
    for (bool NoOpt : {false, true}) {
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(NoOpt);
        SCOPED_TRACE(LLVM);
        auto Image = bridgeImage();
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Entry};
        if (Typed) {
          Image.Symbols[0].Name = "ZwQueryVirtualMemory";
          // An independently encoded caller exercises the ordinary pipeline's
          // register/stack recovery and whole-module declarations together.
          std::vector<uint8_t> Bytes = {0x48, 0x83, 0xec, 0x38, 0x48, 0xb9};
          const auto Immediate = [&](uint64_t Value, unsigned Count) {
            for (unsigned I = 0; I < Count; ++I)
              Bytes.push_back(Value >> (8 * I));
          };
          Immediate(0x1234567812345678ULL, 8);
          Bytes.push_back(0xba);
          Immediate(0xfedcba98, 4);
          Bytes.insert(Bytes.end(), {0x41, 0xb8});
          Immediate(0x12340, 4);
          Bytes.insert(Bytes.end(), {0x41, 0xb9});
          Immediate(0xbc, 4);
          Bytes.insert(Bytes.end(), {0x48, 0xb8});
          Immediate(0x9876543212345678ULL, 8);
          Bytes.insert(Bytes.end(), {0x48, 0x89, 0x44, 0x24, 0x20, 0xb8});
          Immediate(0xf1234567, 4);
          Bytes.insert(Bytes.end(), {0x48, 0x89, 0x44, 0x24, 0x28});
          Bytes.push_back(0xe8);
          Immediate(Entry - (Caller + Bytes.size() + 4), 4);
          Bytes.insert(Bytes.end(), {0x48, 0x83, 0xc4, 0x38, 0xc3});
          std::copy(Bytes.begin(), Bytes.end(),
                    Image.Segments[0].Data.begin() + Caller - Linkage);
          auto Symbol = Symbol::makeFunc(Caller);
          Symbol.Name = "bridge_caller";
          Image.Symbols.push_back(std::move(Symbol));
          Options.OnlyFunctionEntries.insert(Caller);
        }
        Options.NoOpt = NoOpt;
        Options.LiftMode = LLVM;
        Options.SourceProjection = true;
        auto Result = Pipeline().run(Image, Context, Options, Debug);
        ASSERT_TRUE(Result.Success) << Result.Error;
        if (Typed) {
          const auto Call =
              std::find_if(Result.MedFuncs.begin(), Result.MedFuncs.end(),
                           [](const MedFunc &F) { return F.Entry == Caller; });
          ASSERT_NE(Call, Result.MedFuncs.end());
          ASSERT_EQ(Call->CallInfos.size(), 1u);
          ASSERT_EQ(Call->CallInfos[0].Args.size(), 6u);
        }
        CEmitterOptions Emit;
        Emit.Image = &Image;
        Emit.TheArch = Arch::X64;
        Emit.Format = BinaryFormat::COFF;
        Emit.EmitIncludes = true;
        std::string Text = "#include <stdint.h>\n#include <stddef.h>\n";
        llvm::raw_string_ostream Stream(Text);
        if (LLVM) {
          ASSERT_NE(Result.LlvmModule, nullptr);
          ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Stream, Emit,
                                          Debug, &Image));
        } else {
          ASSERT_EQ(Result.HighFuncs.size(), Typed ? 2u : 1u);
          ASSERT_TRUE(
              HighCEmitter().emit(Result.HighFuncs, Stream, Emit, Debug));
        }
        Stream.flush();
        ASSERT_NE(Text.find("neverd.native-bridge"), std::string::npos) << Text;
        EXPECT_NE(Text.find("naked, ms_abi"), std::string::npos);
        EXPECT_NE(Text.find("4045620583"), std::string::npos);
        EXPECT_EQ(Text.find("unknown register"), std::string::npos);
        EXPECT_EQ(Text.find("unknown value"), std::string::npos);
        EXPECT_EQ(Text.find("native_dispatch("), std::string::npos) << Text;
        if (Typed)
          EXPECT_NE(Text.find("int32_t ZwQueryVirtualMemory("),
                    std::string::npos)
              << Text;
        const auto Source = tmpFile("bridge.c");
        std::ofstream(Source) << "#define BRIDGE_TYPED " << Typed << "\n"
                              << Text << "\n#include \"" << TEST_SOURCE_DIR
                              << "/x86_64/fixtures/x64_dispatch_frame.c\"\n";
        for (const char *Optimization : {"O0", "O2"}) {
          SCOPED_TRACE(Optimization);
#if defined(__linux__) && defined(__x86_64__)
          const auto Ran =
              exec("python3",
                   {std::string(TEST_SOURCE_DIR) +
                        "/../devirtualization/run_x64_continuation_kvm.py",
                    "--clang", NEVERD_TEST_CLANG, "--source", Source.string(),
                    "--optimization", Optimization});
          if (Ran.exitCode == 77)
            GTEST_SKIP() << Ran.err;
          ASSERT_TRUE(Ran.ok()) << Ran.err << " exit " << Ran.exitCode << Text;
#else
          const auto Built =
              exec(NEVERD_TEST_CLANG,
                   {"--target=x86_64-linux-gnu",
                    std::string("-") + Optimization, "-ffreestanding", "-c",
                    Source.string(), "-o", tmpFile("bridge.o").string()});
          ASSERT_TRUE(Built.ok()) << Built.err << Text;
#endif
        }
        if (LLVM) {
          auto *Function = Result.LlvmModule->getFunction(
              Typed ? "ZwQueryVirtualMemory" : "bridge_entry");
          ASSERT_NE(Function, nullptr);
          Image.Segments[0].Data[Entry - Linkage + 21] ^= 1;
          EXPECT_THROW(LLVMCEmitter().emit(*Result.LlvmModule, Stream, Emit,
                                           Debug, &Image),
                       std::invalid_argument);
          Image.Segments[0].Data[Entry - Linkage + 21] ^= 1;
          Function->getEntryBlock().front().eraseFromParent();
          EXPECT_THROW(LLVMCEmitter().emit(*Result.LlvmModule, Stream, Emit,
                                           nullptr, &Image),
                       std::invalid_argument);
        }
      }
    }
  }
}
} // namespace
