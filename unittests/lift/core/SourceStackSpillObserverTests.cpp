//===- SourceStackSpillObserverTests.cpp - Settled scratch restores -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare original instructions and C after an opaque frame-pointer borrow.
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace {
using namespace neverd;
constexpr va_t Entry = 0x10000;
constexpr va_t ImportSlot = 0x20000;

class FrameObserverHeader : public NullDebugContext {
public:
  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    if (Name != "frame_observer")
      return std::nullopt;
    FunctionSym Function;
    Function.Name = Name.str();
    Function.PrototypeFromHeader = true;
    Function.PrototypeLibrary = "test-frame-observer";
    Function.CallConv = DebugCallConv::Cdecl;
    Function.ReturnType = NdType::makeInt(4, true);
    Function.Params = {{"frame", NdType::makePtr()}};
    Function.PortablePrototypeDeclaration =
        "int32_t frame_observer(void *frame)";
    return Function;
  }

  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    auto Function = Address == ImportSlot ? resolvePrototype("frame_observer")
                                          : std::nullopt;
    if (Function)
      Function->Addr = Address;
    return Function;
  }
};

BinaryImage fixture(BinaryFormat Format, bool ObserveRestored,
                    va_t Address = Entry) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = Format;
  Image.Entry = Address;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Address;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0x48, 0x83, 0xEC,
               0x28, // sub rsp,40
               0x48, 0x8D, uint8_t(Format == BinaryFormat::COFF ? 0x4C : 0x7C),
               0x24, 0x20,  // lea rcx/rdi,[rsp+32]
               0xFF, 0x15}; // call [rip+disp]
  const uint32_t Displacement = ImportSlot - (Address + 15);
  for (unsigned Byte = 0; Byte != 4; ++Byte)
    Text.Data.push_back(Displacement >> (Byte * 8));
  const uint8_t Body[] = {
      0x52,                                     // push rdx
      0x48, 0x8D, 0x15, 3,    0,    0,  0,      // lea rdx,[rip+3]
      0xFF, 0xE2, 0xE9,                         // jmp rdx; stray E9
      0x5A, 0x9C, 0x9D,                         // pop rdx; pushfq; popfq
      0x48, 0xC7, 0x44, 0x24, 0xF8, 19, 0, 0, 0 // mov [rsp-8],19
  };
  Text.Data.insert(Text.Data.end(), std::begin(Body), std::end(Body));
  if (ObserveRestored)
    Text.Data.insert(Text.Data.end(), {0x48, 0x89, 0xD0}); // mov rax,rdx
  else
    Text.Data.insert(Text.Data.end(), {0xB8, 19, 0, 0, 0}); // mov eax,19
  Text.Data.insert(Text.Data.end(), {0x48, 0x83, 0xC4, 0x28, 0xC3});
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  Segment Iat;
  Iat.Name = ".idata";
  Iat.VA = ImportSlot;
  Iat.Size = Iat.FileSz = sizeof(uint64_t);
  Iat.Flags = SegmentFlags::Readable;
  Iat.Data.resize(sizeof(uint64_t));
  Image.Segments.push_back(std::move(Iat));
  Import Observer;
  Observer.Name = "frame_observer";
  Observer.Module = "fixture.dll";
  Observer.IATAddr = ImportSlot;
  Image.Imports.push_back(std::move(Observer));
  EXPECT_TRUE(Image.recordImportStorageSlot(ImportSlot, "frame_observer", 0,
                                            ImportStorageEvidence::LoaderBind));
  auto Function = Symbol::makeFunc(Address);
  Function.Name = Address == Entry
                      ? "frame_spill_probe"
                      : "frame_spill_probe_" + std::to_string(Address);
  Image.Symbols.push_back(std::move(Function));
  return Image;
}

std::string nativeOracle(const BinaryImage &Image) {
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  Out << "\n#include <sys/mman.h>\n#include <string.h>\n"
         "static volatile uint64_t native_calls;\n"
         "static void *volatile native_pointer;\n"
         "__attribute__((naked)) static uint32_t native_observer(void *p) {\n"
         "  __asm__(\"incq native_calls(%rip); movq %"
      << (Image.Format == BinaryFormat::COFF ? "rcx" : "rdi")
      << ",native_pointer(%rip); movabsq $0x1020304050607080,%rdx; "
         "movl $1,%eax; retq\");\n}\n"
         "static uint64_t native_probe(void) {\n"
         "  static const unsigned char original[] = {";
  for (uint8_t Byte : Image.Segments.front().Data)
    Out << unsigned(Byte) << ',';
  Out << "};\n  const size_t size = " << ImportSlot - Entry + sizeof(uint64_t)
      << ";\n"
         "  unsigned char *mapping = mmap(0,size+16,"
         "PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);\n"
         "  if (mapping == MAP_FAILED) __builtin_trap();\n"
         "  unsigned char *code = mapping+16;\n"
         "  memcpy(code,original,sizeof(original));\n"
         "  uintptr_t target = (uintptr_t)native_observer;\n"
         "  memcpy(code+"
      << ImportSlot - Entry
      << ",&target,8);\n"
         "  __builtin___clear_cache((char *)code,(char *)code+size);\n"
         "  uint64_t value = ((uint64_t ("
      << (Image.Format == BinaryFormat::COFF ? "__attribute__((ms_abi)) " : "")
      << "*)(void))code)();\n"
         "  if (munmap(mapping,size+16)) __builtin_trap();\n"
         "  return value;\n}\n";
  return Source;
}

TEST(SourceStackSpillObservers, DeadRestoresMatchOriginalAcrossBothRoutes) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "the original x64 observer requires Linux x64 mmap";
#endif
  FrameObserverHeader Header;
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool ObserveRestored : {false, true})
      for (bool LLVM : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << unsigned(Format) << ':' << ObserveRestored << ':'
                       << LLVM << ':' << NoOpt);
          const auto Image = fixture(Format, ObserveRestored);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.OnlyFunctionEntries = {Entry};
          Options.EmitDumpOutput = false;
          Options.SourceProjection = true;
          Options.LiftMode = LLVM;
          Options.NoOpt = NoOpt;
          auto Result = Pipeline().run(Image, Context, Options, &Header);
          ASSERT_TRUE(Result.Success) << Result.Error;
          std::string Source = R"C(
#include <stdint.h>
#include <stddef.h>
static inline uint64_t __readeflags(void) {
  uint64_t x; __asm__ volatile("pushfq; popq %0" : "=r"(x) : : "memory");
  return x;
}
static inline void __writeeflags(uint64_t x) {
  __asm__ volatile("pushq %0; popfq" : : "r"(x) : "cc", "memory");
}
static volatile unsigned source_calls;
static void *volatile source_pointer;
__attribute__((noinline)) int32_t frame_observer(void *frame) {
  ++source_calls; source_pointer = frame;
  __asm__ volatile("movabsq $0x1020304050607080, %%rdx" : : : "rdx");
  return 1;
}
)C";
          llvm::raw_string_ostream Out(Source);
          CEmitterOptions COptions;
          COptions.EmitIncludes = false;
          COptions.TheArch = Image.Arch;
          COptions.Format = Format;
          COptions.Image = &Image;
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                            &Header, &Image));
          } else
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Header));
          EXPECT_EQ(Source.find("__builtin_trap") != std::string::npos,
                    ObserveRestored)
              << Source;
          if (ObserveRestored)
            continue;
          Out << nativeOracle(Image) << R"C(
int main(void) {
  uint64_t original = native_probe();
  if (original != 19 || native_calls != 1 || !native_pointer) return 1;
  if ((uint64_t)frame_spill_probe() != original || source_calls != 1 ||
      !source_pointer) return 2;
  return 0;
}
)C";
          for (const char *Level : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                Source, {Level, "-D_GNU_SOURCE", "-fno-strict-aliasing",
                         "-fsanitize=undefined", "-fsanitize-trap=all"});
        }
}

TEST(SourceStackSpillObservers, SettledBatchDoesNotDependOnWorkerCount) {
  const unsigned Previous =
      workerThreadOverride().load(std::memory_order_relaxed);
  const llvm::scope_exit RestoreThreads(
      [&] { setWorkerThreadCount(Previous); });
  FrameObserverHeader Header;
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    BinaryImage Image = fixture(Format, false);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.SourceProjection = true;
    Options.OnlyFunctionEntries = {Entry};
    for (size_t I = 1; I <= limits::kMinParallelIRWorkItems; ++I) {
      const va_t Address = Entry + I * 0x100;
      auto Next = fixture(Format, false, Address);
      Image.Segments.push_back(std::move(Next.Segments.front()));
      Image.Symbols.push_back(std::move(Next.Symbols.front()));
      Options.OnlyFunctionEntries.insert(Address);
    }
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(Format) << ':' << LLVM << ':' << NoOpt);
        Options.LiftMode = LLVM;
        Options.NoOpt = NoOpt;
        std::string Serial;
        for (unsigned Threads : {1u, 4u}) {
          setWorkerThreadCount(Threads);
          llvm::LLVMContext Context;
          auto Result = Pipeline().run(Image, Context, Options, &Header);
          ASSERT_TRUE(Result.Success) << Result.Error;
          ASSERT_EQ(Result.MedFuncs.size(), Options.OnlyFunctionEntries.size());
          CEmitterOptions COptions;
          COptions.EmitIncludes = false;
          COptions.TheArch = Image.Arch;
          COptions.Format = Format;
          COptions.Image = &Image;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                            &Header, &Image));
          } else
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Header));
          ASSERT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
          if (Threads == 1)
            Serial = Source;
          else
            EXPECT_EQ(Source, Serial);
        }
      }
  }
}
} // namespace
