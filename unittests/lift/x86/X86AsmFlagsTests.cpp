//===- X86AsmFlagsTests.cpp - Flags across native instructions ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute compiler-owned conditions across architectural ZF writes. Null
/// selectors fail VERR, VERW, LAR and LSL without privileged execution.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/X86/X86DescriptorStateNative.h"
#include "neverd/backend/llvm/X86/X86SgxAsm.h"
#include "neverd/backend/llvm/X86/X86SystemStateAsm.h"
#include "neverd/backend/llvm/X86/X86VirtualizationAsm.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

#include <array>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <vector>

using namespace neverd;

namespace {
constexpr std::array Names = {"verr", "verw", "lar", "lsl"};

void addProbes(llvm::Module &M, Arch A) {
  llvm::IRBuilder<> B(M.getContext());
  for (unsigned Kind = 0; Kind < Names.size(); ++Kind)
    for (bool Memory : {false, true}) {
      const std::string Name =
          std::string(Names[Kind]) + (Memory ? "_mem" : "_reg");
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(B.getInt32Ty(),
                                  {B.getInt32Ty(), B.getInt32Ty()}, false),
          llvm::GlobalValue::ExternalLinkage, Name, M);
      B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", F));
      auto *Equal = B.CreateICmpEQ(F->getArg(0), F->getArg(1));
      // Both are null selectors. The select forces the first use of Equal
      // before the native instruction, which clears ZF for a null selector.
      llvm::Value *Selector =
          B.CreateSelect(Equal, B.getInt16(0), B.getInt16(3));
      if (Memory) {
        auto *Slot = B.CreateAlloca(B.getInt16Ty());
        B.CreateStore(Selector, Slot);
        Selector = Slot;
      }
      std::string Text, Constraints;
      llvm::Type *ResultType = B.getInt8Ty();
      if (Kind < 2) {
        auto Plan = x86DescriptorStateNative(
            Kind == 0 ? Intrinsic::Verr : Intrinsic::Verw, A, Memory, 1);
        ASSERT_TRUE(Plan);
        Text = Plan->Text;
        Constraints = Plan->Constraints;
      } else {
        const Intrinsic Id =
            Kind == 2 ? (Memory ? Intrinsic::X86LoadAccessRightsMemory
                                : Intrinsic::X86LoadAccessRights)
                      : (Memory ? Intrinsic::X86LoadSegmentLimitMemory
                                : Intrinsic::X86LoadSegmentLimit);
        auto Plan = x86SystemStateAsm(Id, A, 4, NdMemoryAddressSpace::Default);
        ASSERT_TRUE(Plan);
        Text = Plan->Text;
        Constraints = Plan->Constraints;
        ResultType = llvm::StructType::get(M.getContext(),
                                           {B.getInt32Ty(), B.getInt8Ty()});
      }
      auto *Asm = llvm::InlineAsm::get(
          llvm::FunctionType::get(ResultType, {Selector->getType()}, false),
          Text, Constraints, true);
      llvm::Value *Flag = B.CreateCall(Asm, {Selector});
      if (Kind >= 2)
        Flag = B.CreateExtractValue(Flag, 1);
      B.CreateRet(B.CreateSelect(Equal, B.CreateZExt(Flag, B.getInt32Ty()),
                                 B.getInt32(-1)));
    }
}

std::string driver(Arch A, bool Declarations) {
  std::string Text;
  if (Declarations)
    for (const char *Name : Names)
      for (const char *Form : {"_reg", "_mem"})
        Text += "extern unsigned " + std::string(Name) + Form +
                "(unsigned, unsigned);\n";
  Text += R"c(
static unsigned check(void) {
  static const unsigned values[] = {0, 1, 2, 0xffffffffu, 0x80000000u,
                             0x7fffffffu, 0x12345678u, 0xa5a5a5a5u};
  for (unsigned i = 0; i < 8; ++i)
    for (unsigned j = 0; j < 8; ++j) {
      const unsigned expected = i == j ? 0 : 0xffffffffu;
)c";
  for (const char *Name : Names)
    for (const char *Form : {"_reg", "_mem"})
      Text += "if (" + std::string(Name) + Form +
              "(values[i], values[j]) != expected) return 1;\n";
  Text += "} return 0; }\n";
  if (A == Arch::X86)
    Text += R"c(
__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
  unsigned result = check();
  __asm__ __volatile__("int $0x80" : : "a"(1), "b"(result) : "memory");
  __builtin_unreachable();
}
)c";
  else
    Text += "int main(void) { return (int)check(); }\n";
  return Text;
}

void writeFile(const std::string &Path, llvm::StringRef Text) {
  std::error_code Error;
  llvm::raw_fd_ostream File(Path, Error);
  ASSERT_FALSE(Error);
  File << Text;
}

void execute(const std::string &Compiler, const std::string &Directory, Arch A,
             llvm::StringRef Level, const std::string &Source,
             const std::string &Additional = {}, bool NeverC = false) {
  const auto Input = Directory + "/driver.c";
  const auto Output = Directory + "/probe";
  const auto Errors = Directory + "/errors.txt";
  writeFile(Input, Source);
  llvm::SmallVector<llvm::StringRef, 16> Args{Compiler, "-std=c11", Level,
                                              Input,    "-o",       Output};
  if (!Additional.empty())
    Args.push_back(Additional);
  if (A == Arch::X86)
    Args.append(
        {"-m32", "-ffreestanding", "-nostdlib", "-static", "-Wl,-e,_start"});
  if (NeverC)
    Args.append({"-fno-lto", "-fno-builtin-mimalloc"});
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                      std::nullopt, Errors};
  std::string Error;
  const int Result = llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt,
                                               Redirects, 30, 0, &Error);
  auto Log = llvm::MemoryBuffer::getFile(Errors);
  ASSERT_EQ(Result, 0) << Error << (Log ? (*Log)->getBuffer().str() : "");
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}, std::nullopt, Redirects,
                                      30, 0, &Error),
            0)
      << Error << Source;
}

TEST(X86AsmFlags, CompilerConditionsSurviveDescriptorFlagWrites) {
  const llvm::Triple Host(llvm::sys::getDefaultTargetTriple());
  if (Host.getArch() != llvm::Triple::x86_64 || !Host.isOSLinux())
    GTEST_SKIP() << "native x64 and freestanding i386 probes require Linux x64";
  const auto Compiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(Compiler) << "clang is required to link native probes";
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-asm-flags", Temporary));
  const auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Temporary); });
  const std::string Directory(Temporary.str());
  for (auto A : {Arch::X86, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    llvm::LLVMContext Context;
    llvm::Module M("descriptor-flags", Context);
    M.setTargetTriple(llvm::Triple(A == Arch::X64 ? "x86_64-unknown-linux-gnu"
                                                  : "i386-unknown-linux-gnu"));
    M.setDataLayout(A == Arch::X64 ? "e-p:64:64-i64:64" : "e-p:32:32-i64:64");
    addProbes(M, A);
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    std::string IR, Source;
    llvm::raw_string_ostream IROS(IR), SourceOS(Source);
    M.print(IROS, nullptr);
    CEmitterOptions Options;
    Options.TheArch = A;
    Options.Format = BinaryFormat::ELF;
    Options.PreserveLLVMFunctionTypes = true;
    ASSERT_TRUE(LLVMCEmitter().emit(M, SourceOS, Options));
    auto Object = Codegen().compile(M, A, BinaryFormat::ELF);
    ASSERT_TRUE(Object.Success);
    const auto ObjectPath = Directory + "/probe.o";
    const auto IRPath = Directory + "/probe.ll";
    writeFile(ObjectPath, llvm::StringRef(reinterpret_cast<const char *>(
                                              Object.ObjectData.data()),
                                          Object.ObjectData.size()));
    writeFile(IRPath, IR);
    for (llvm::StringRef Level : {"-O0", "-O2"}) {
      SCOPED_TRACE(Level.str());
      execute(*Compiler, Directory, A, Level, driver(A, true), ObjectPath);
      execute(*Compiler, Directory, A, Level, driver(A, true), IRPath);
      execute(*Compiler, Directory, A, Level, Source + driver(A, false));
      if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
          A == Arch::X64 && NeverC)
        execute(NeverC, Directory, A, Level, Source + driver(A, false), {},
                true);
      ASSERT_FALSE(::testing::Test::HasFailure());
    }
    if (const char *Exports = std::getenv("NEVERD_ASM_FLAGS_EXPORT_DIR")) {
      ASSERT_FALSE(llvm::sys::fs::create_directories(Exports));
      const auto Stem = std::string(Exports) + (A == Arch::X64 ? "/64" : "/32");
      writeFile(Stem + ".ll", IR);
      writeFile(Stem + ".c", Source);
      writeFile(Stem + ".o", llvm::StringRef(reinterpret_cast<const char *>(
                                                 Object.ObjectData.data()),
                                             Object.ObjectData.size()));
    }
  }
}

TEST(X86AsmFlags, LLVMToCRejectsIgnoredConditionCodeClobbers) {
  for (auto A : {Arch::X86, Arch::X64}) {
    llvm::LLVMContext Context;
    llvm::Module M("ignored-condition-code", Context);
    addProbes(M, A);
    for (auto &F : M)
      for (auto &Block : F)
        for (auto &Instruction : Block)
          if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction)) {
            auto *Asm = llvm::cast<llvm::InlineAsm>(Call->getCalledOperand());
            auto Constraints = Asm->getConstraintString().str();
            const auto At = Constraints.find("~{flags}");
            ASSERT_NE(At, std::string::npos);
            Constraints.replace(At, 8, "~{cc}");
            Call->setCalledOperand(llvm::InlineAsm::get(Asm->getFunctionType(),
                                                        Asm->getAsmString(),
                                                        Constraints, true));
          }
    CEmitterOptions Options;
    Options.TheArch = A;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    EXPECT_THROW(LLVMCEmitter().emit(M, OS, Options), std::exception);
  }
}

TEST(X86AsmFlags, CompileOwnedFlagWritingContractsForMachineEffectAudit) {
  const Intrinsic System[] = {
#define NEVERD_X86_SYSTEM_STATE(Id, ...) Intrinsic::Id,
#define NEVERD_X86_EXISTING_SYSTEM_STATE NEVERD_X86_SYSTEM_STATE
#define NEVERD_X86_APPENDED_SYSTEM_STATE NEVERD_X86_SYSTEM_STATE
#include "neverd/ir/intrinsics/X86/X86SystemState.def"
#undef NEVERD_X86_APPENDED_SYSTEM_STATE
#undef NEVERD_X86_EXISTING_SYSTEM_STATE
#undef NEVERD_X86_SYSTEM_STATE
  };
  const Intrinsic Virtualization[] = {
#define NEVERD_X86_VIRTUALIZATION(Id, ...) Intrinsic::Id,
#include "neverd/ir/intrinsics/X86/X86Virtualization.def"
#undef NEVERD_X86_VIRTUALIZATION
  };
  for (auto A : {Arch::X86, Arch::X64}) {
    const unsigned Word = A == Arch::X64 ? 8 : 4;
    llvm::LLVMContext Context;
    llvm::Module M("owned-flags-inventory", Context);
    M.setTargetTriple(llvm::Triple(A == Arch::X64 ? "x86_64-unknown-linux-gnu"
                                                  : "i386-unknown-linux-gnu"));
    llvm::IRBuilder<> B(Context);
    unsigned Count = 0;
    auto Add = [&](llvm::StringRef Text, llvm::StringRef Constraints,
                   const std::vector<uint16_t> &Inputs,
                   const std::vector<uint16_t> &Outputs) {
      llvm::SmallVector<llvm::Type *, 4> InputTypes, OutputTypes;
      for (unsigned Bytes : Inputs)
        InputTypes.push_back(B.getIntNTy(8 * Bytes));
      for (unsigned Bytes : Outputs)
        OutputTypes.push_back(B.getIntNTy(8 * Bytes));
      llvm::Type *Result = OutputTypes.empty() ? B.getVoidTy() : OutputTypes[0];
      if (OutputTypes.size() > 1)
        Result = llvm::StructType::get(Context, OutputTypes);
      auto *F = llvm::Function::Create(
          llvm::FunctionType::get(B.getVoidTy(), InputTypes, false),
          llvm::GlobalValue::ExternalLinkage,
          "effect_" + std::to_string(Count++), M);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
      llvm::SmallVector<llvm::Value *, 4> Arguments;
      for (auto &Arg : F->args())
        Arguments.push_back(&Arg);
      B.CreateCall(llvm::InlineAsm::get(
                       llvm::FunctionType::get(Result, InputTypes, false), Text,
                       Constraints, true),
                   Arguments);
      B.CreateRetVoid();
    };
    for (auto Id : {Intrinsic::X86Encls, Intrinsic::X86Enclv}) {
      const auto P = x86SgxAsm(Id, A);
      ASSERT_TRUE(P);
      const auto W = static_cast<uint16_t>(Word);
      Add(P->Text, P->Constraints, {W, W, W, W}, {W, W, 2});
    }
    for (auto Id : Virtualization)
      for (auto Space :
           {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
            NdMemoryAddressSpace::X86GS}) {
        const auto P = x86VirtualizationAsm(Id, A, Space, Word);
        if (!P || x86VirtualizationContract(Id).Result ==
                      X86VirtualizationResult::None)
          continue;
        std::vector<uint16_t> Outputs;
        if (x86VirtualizationContract(Id).Result ==
            X86VirtualizationResult::Read)
          Outputs.push_back(Word);
        Outputs.push_back(2);
        Add(P->Text, P->Constraints, P->InputBytes, Outputs);
      }
    for (auto Id : System)
      for (uint16_t Width : {0, 2, 4, 8})
        for (auto Space :
             {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
              NdMemoryAddressSpace::X86GS}) {
          const auto P = x86SystemStateAsm(Id, A, Width, Space);
          if (P && P->ChangesFlags)
            Add(P->Text, P->Constraints, P->InputBytes, P->OutputBytes);
        }
    for (uint16_t Selector = 0; Selector < X86SegmentedBlockSelectorLimit;
         ++Selector) {
      const auto P = x86SystemStateAsm(Intrinsic::X86SegmentedBlockMemory, A, 0,
                                       NdMemoryAddressSpace::Default, Selector);
      if (P && P->ChangesFlags)
        Add(P->Text, P->Constraints, P->InputBytes, P->OutputBytes);
    }
    for (auto Id : {Intrinsic::Verr, Intrinsic::Verw})
      for (bool Memory : {false, true}) {
        const auto P = x86DescriptorStateNative(Id, A, Memory, 1);
        ASSERT_TRUE(P);
        Add(P->Text, P->Constraints, {static_cast<uint16_t>(P->OperandBytes)},
            {1});
      }
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    if (const char *Exports = std::getenv("NEVERD_ASM_FLAGS_EXPORT_DIR")) {
      ASSERT_FALSE(llvm::sys::fs::create_directories(Exports));
      std::string IR;
      llvm::raw_string_ostream OS(IR);
      M.print(OS, nullptr);
      writeFile(std::string(Exports) +
                    (Word == 8 ? "/inventory-64.ll" : "/inventory-32.ll"),
                IR);
    }
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(static_cast<unsigned>(Format));
      EXPECT_TRUE(Codegen().compile(M, A, Format).Success);
    }
    RecordProperty(Word == 8 ? "x64_flag_writing_contracts"
                             : "i386_flag_writing_contracts",
                   Count);
  }
}
} // namespace
