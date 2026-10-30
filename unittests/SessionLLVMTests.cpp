//===- SessionLLVMTests.cpp - Private LLVM query/cache contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify private LLVM source queries and their session cache boundaries.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/intrinsics/AArch64/AArch64ExceptionReturn.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Verifier.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sdk {
namespace {

class SessionLLVMTest : public ::testing::Test {
protected:
  void SetUp() override {
    State.Loaded = true;
    State.PipeRan = true;
    State.PipeResult.Success = true;
    State.Img.Arch = Arch::X64;
    State.Img.Bits = Bitness::Bits64;
    State.Img.Format = BinaryFormat::ELF;
    State.LLVMCtx = std::make_unique<llvm::LLVMContext>();
  }

  llvm::Function *addFunction(const std::string &Name,
                              std::optional<va_t> Address,
                              bool Declaration = false) {
    if (!State.PipeResult.LlvmModule)
      State.PipeResult.LlvmModule =
          std::make_unique<llvm::Module>("identity_fixture", *State.LLVMCtx);
    auto *Type =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*State.LLVMCtx), false);
    auto *Function =
        llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage, Name,
                               *State.PipeResult.LlvmModule);
    if (Address)
      rewrite_source::setOriginalVA(*Function, *Address);
    if (!Declaration) {
      auto *Block = llvm::BasicBlock::Create(*State.LLVMCtx, "entry", Function);
      llvm::ReturnInst::Create(*State.LLVMCtx, Block);
    }
    return Function;
  }

  MedFunc sourceFunction() {
    MedFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "source_function";
    Function.ReturnType = NdType::makeVoid();
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(Return);
    Function.Blocks.push_back(std::move(Block));
    return Function;
  }

  Session State;
};

TEST_F(SessionLLVMTest, OmittedDebugNamedRuntimeUsesItsDeclaredArity) {
  constexpr va_t Runtime = 0x2000;
  class PublicNames : public NullDebugContext {
  public:
    bool hasInfo() const override { return true; }
    std::optional<std::string> functionName(va_t Address) const override {
      return Address == Runtime
                 ? std::optional<std::string>("__security_check_cookie")
                 : std::nullopt;
    }
  };
  State.Dbg = std::make_unique<PublicNames>();
  State.Img.Format = BinaryFormat::COFF;
  MedFunc Function = sourceFunction();
  Function.CC = CallingConv::Win64;
  auto &Ops = Function.Blocks.front().Ops;
  Ops.clear();
  unsigned Id = 0;
  for (uint64_t Register : {x86reg::RCX, x86reg::RDX, x86reg::R8}) {
    MedOp Setup;
    Setup.Opcode = NdOp::COPY;
    Setup.Addr = Function.Entry + Id;
    Setup.Output.Kind = MedVar::Reg;
    Setup.Output.Id = ++Id;
    Setup.Output.RegOff = Register;
    Setup.Output.Size = 8;
    Setup.Output.SSAVer = 1;
    Setup.Output.TheArch = Arch::X64;
    Setup.addInput(MedVar::makeConst(47 + Id, 8));
    Ops.push_back(Setup);
  }
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Function.Entry + Id;
  Call.addInput(MedVar::makeConst(Runtime, 8));
  Ops.push_back(Call);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Ops.push_back(Return);
  State.PipeResult.MedFuncs.push_back(std::move(Function));
  std::shared_ptr<LLVMSourceMap> Sources;
  for (bool NoOpt : {false, true}) {
    auto Module = State.emitNativeLlvm(NoOpt, nullptr, Sources);
    ASSERT_TRUE(Module) << State.LastError;
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    const auto *RuntimeFunction =
        Module->getFunction("__security_check_cookie");
    ASSERT_TRUE(RuntimeFunction);
    EXPECT_EQ(RuntimeFunction->arg_size(), 1u);
    ASSERT_TRUE(State.NativeMedFuncs);
    const auto &Calls = State.NativeMedFuncs->front().CallInfos;
    ASSERT_EQ(Calls.size(), 1u);
    EXPECT_EQ(Calls.front().Args.size(), 1u);
  }
}

TEST_F(SessionLLVMTest, ExactAddressZeroIgnoresHelperNamesAndDeclarations) {
  addFunction("sub_0", std::nullopt);
  addFunction("external_declaration", 0, true);
  const auto *Source = addFunction("debug_named_entry", 0);
  addFunction("another_source", 0x1000);

  EXPECT_EQ(State.findNativeLlvmFunction(0), Source);
  EXPECT_TRUE(State.LastError.empty());
}

TEST_F(SessionLLVMTest, CallRecoveryKeepsUnobservedSourceSpillArguments) {
  constexpr va_t Caller = 0x1000, Consumer = 0x1040, Global = 0x2000;
  State.Img.Format = BinaryFormat::COFF;
  State.Img.Entry = Caller;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Caller;
  Text.Size = Text.FileSz = 0x80;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xCC);
  const std::vector<uint8_t> CallBytes = {0x48, 0x83, 0xEC, 0x28, 0xE8,
                                          0x37, 0,    0,    0,    0x48,
                                          0x83, 0xC4, 0x28, 0xC3};
  const std::vector<uint8_t> Body = {
      0x48, 0x89, 0x4C, 0x24, 8,            // mov [rsp+8],rcx
      0x53, 0x48, 0x83, 0xEC, 0x20,         // push rbx; sub rsp,20h
      0x48, 0x8B, 0x1D, 0xAF, 0x0F, 0,   0, // mov rbx,[rip+global]
      0x48, 0x83, 0x64, 0x24, 0x30, 0,      // and qword [rsp+30h],0
      0x48, 0x8D, 0x4C, 0x24, 0x30,         // lea rcx,[rsp+30h]
      0xFF, 0x15, 0x9E, 0x1F, 0,    0,      // call [strlen]
      0x48, 0x01, 0xD8,                     // add rax,rbx
      0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};  // add rsp,20h; pop rbx; ret
  std::copy(CallBytes.begin(), CallBytes.end(), Text.Data.begin());
  std::copy(Body.begin(), Body.end(), Text.Data.begin() + Consumer - Caller);
  State.Img.Segments.push_back(std::move(Text));
  Segment Data;
  Data.Name = ".data";
  Data.VA = Global;
  Data.Size = Data.FileSz = 8;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.assign(8, 0);
  State.Img.Segments.push_back(std::move(Data));
  Segment Slots;
  Slots.Name = ".idata";
  Slots.VA = 0x3000;
  Slots.Size = Slots.FileSz = 16;
  Slots.Flags = SegmentFlags::Readable;
  Slots.Data.assign(16, 0);
  State.Img.Segments.push_back(std::move(Slots));
  Import Reader;
  Reader.Name = "strlen";
  Reader.Module = "msvcrt.dll";
  Reader.IATAddr = 0x3000;
  State.Img.Imports.push_back(std::move(Reader));
  Import Observer;
  Observer.Name = "rand";
  Observer.Module = "msvcrt.dll";
  Observer.IATAddr = 0x3008;
  State.Img.Imports.push_back(std::move(Observer));
  for (const auto &[Entry, Name] : {std::pair{Caller, "spill_caller"},
                                    std::pair{Consumer, "spill_consumer"}}) {
    auto Symbol = Symbol::makeFunc(Entry);
    Symbol.Name = Name;
    State.Img.Symbols.push_back(std::move(Symbol));
  }
  Symbol Object;
  Object.Addr = Global;
  Object.Name = "spill_global";
  Object.Size = 8;
  State.Img.Symbols.push_back(std::move(Object));
  const auto OriginalImage = State.Img;
  for (unsigned Variant : {0, 1, 2, 3, 4}) {
    SCOPED_TRACE(Variant);
    const bool ThroughWrapper = Variant != 0;
    State.Img = OriginalImage;
    if (ThroughWrapper) {
      auto &Code = State.Img.Segments.front().Data;
      const std::vector<uint8_t> CallerCode = {
          0x48, 0x83, 0xEC, 0x28,    // sub rsp,28h
          0xB1, 0x5A,                // mov cl,5Ah
          0xE8, 0x15, 0,    0,    0, // call wrapper
          0x48, 0x83, 0xC4, 0x28, 0xC3};
      std::vector<uint8_t> Wrapper = {
          0x53,                      // push rbx
          0x0F, 0xB6, 0xD9,          // movzx ebx,cl
          0x48, 0x83, 0xEC, 0x20,    // sub rsp,20h
          0xE8, 0x13, 0,    0,    0, // call consumer
          0x48, 0x01, 0xD8,          // add rax,rbx
          0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
      if (Variant >= 2) {
        Wrapper.resize(13);
        Wrapper.insert(Wrapper.end(),
                       {0x48, 0x01, 0xC3,             // add rbx,rax
                        0xFF, 0x15, 0xD2, 0x1F, 0, 0, // call [rand]
                        0x48, 0x89, 0xD8,             // mov rax,rbx
                        0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3});
      }
      if (Variant == 4) {
        // A full-width forward has no local spill or narrow entry view.
        Wrapper.erase(Wrapper.begin() + 1, Wrapper.begin() + 4);
        Wrapper[6] += 3;    // consumer rel32
        Wrapper[11] = 0x89; // mov rbx,rax instead of adding an old rbx
        Wrapper[15] += 3;   // rand IAT rel32
      }
      std::copy(CallerCode.begin(), CallerCode.end(), Code.begin());
      std::copy(Wrapper.begin(), Wrapper.end(), Code.begin() + 0x20);
      auto Symbol = Symbol::makeFunc(Caller + 0x20);
      Symbol.Name = "spill_prefix_forwarder";
      State.Img.Symbols.push_back(std::move(Symbol));
      if (Variant >= 3) {
        // Both arms destroy the incoming RCX before strlen, so a later
        // opaque call in the wrapper cannot revive its unused high bytes.
        auto BranchingBody = Body;
        BranchingBody.insert(
            BranchingBody.begin() + 23,
            {0xF6, 0xC3, 1, 0x74, 7, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xEB, 5});
        BranchingBody[42] -= 12; // adjust the strlen IAT displacement
        std::copy(BranchingBody.begin(), BranchingBody.end(),
                  Code.begin() + Consumer - Caller);
      }
    }
    for (bool Omitted : {false, true}) {
      SCOPED_TRACE(Omitted);
      State.NativeMedFuncs.reset();
      State.FunctionLlvmModules.clear();
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Caller};
      if (!Omitted) {
        Options.OnlyFunctionEntries.insert(Consumer);
        if (ThroughWrapper)
          Options.OnlyFunctionEntries.insert(Caller + 0x20);
      }
      State.PipeResult = Pipeline().run(State.Img, *State.LLVMCtx, Options);
      ASSERT_TRUE(State.PipeResult.Success) << State.PipeResult.Error;
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(NoOpt);
        const auto *Shown = State.ensureFunctionLlvmModule(Caller, NoOpt, true);
        ASSERT_TRUE(Shown) << State.LastError;
        ASSERT_TRUE(State.NativeMedFuncs);
        const auto &Calls = State.NativeMedFuncs->front().CallInfos;
        ASSERT_EQ(Calls.size(), 1U);
        ASSERT_EQ(Calls.front().Args.size(), 1U);
        EXPECT_TRUE(Calls.front().Args.front().isConst());
        EXPECT_EQ(Calls.front().Args.front().ConstVal,
                  ThroughWrapper && Variant != 4 ? 0x5AU : 0U);
        EXPECT_EQ(State.ensureFunctionLlvmModule(Caller, NoOpt, true), Shown);
      }
    }
  }
}

TEST_F(SessionLLVMTest, CallRecoveryKeepsUnobservedStackArgumentHoles) {
  constexpr va_t Caller = 0x1000, Wrapper = 0x1080;
  State.Img.Format = BinaryFormat::COFF;
  State.Img.Entry = Caller;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Caller;
  Text.Size = Text.FileSz = 0xC0;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xCC);
  const std::vector<uint8_t> CallBytes = {
      0x48, 0x83, 0xEC, 0x38, 0x48, 0x89, 0x4C, 0x24, 0x28, 0x48, 0x89, 0x54,
      0x24, 0x30, 0xE8, 0x6D, 0,    0,    0,    0x48, 0x83, 0xC4, 0x38, 0xC3};
  std::vector<uint8_t> Body = {
      0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x5C, 0x24,
      0x58, 0xF6, 0xC1, 0x01,          // test cl,1
      0x74, 0x07,                      // jz subtract
      0x48, 0x03, 0x5C, 0x24, 0x60,    // add rbx,[rsp+60h]
      0xEB, 0x05,                      // jmp join
      0x48, 0x2B, 0x5C, 0x24, 0x60,    // sub rbx,[rsp+60h]
      0xFF, 0x15, 0x5F, 0x1F, 0,    0, // call [rand]
      0x48, 0x01, 0xD8, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
  std::copy(CallBytes.begin(), CallBytes.end(), Text.Data.begin());
  std::copy(Body.begin(), Body.end(), Text.Data.begin() + Wrapper - Caller);
  State.Img.Segments.push_back(std::move(Text));
  Segment Slots;
  Slots.Name = ".idata";
  Slots.VA = 0x3000;
  Slots.Size = Slots.FileSz = 8;
  Slots.Flags = SegmentFlags::Readable;
  Slots.Data.assign(8, 0);
  State.Img.Segments.push_back(std::move(Slots));
  Segment Data;
  Data.Name = ".data";
  Data.VA = 0x3100;
  Data.Size = Data.FileSz = 8;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.assign(8, 0);
  State.Img.Segments.push_back(std::move(Data));
  Import Observer;
  Observer.Name = "rand";
  Observer.Module = "msvcrt.dll";
  Observer.IATAddr = 0x3000;
  State.Img.Imports.push_back(std::move(Observer));
  for (const auto &[Entry, Name] :
       {std::pair{Caller, "stack_hole"}, std::pair{Wrapper, "stack_wrapper"}}) {
    auto Symbol = Symbol::makeFunc(Entry);
    Symbol.Name = Name;
    State.Img.Symbols.push_back(std::move(Symbol));
  }
  for (bool ImageRead : {false, true}) {
    SCOPED_TRACE(ImageRead);
    if (ImageRead) {
      // Insert xor rbx,[rip+205Eh] before the import and adjust its
      // displacement.
      Body.insert(Body.begin() + 27, {0x48, 0x33, 0x1D, 0x5E, 0x20, 0, 0});
      Body[36] = 0x58;
      std::copy(Body.begin(), Body.end(),
                State.Img.Segments.front().Data.begin() + Wrapper - Caller);
    }
    for (bool Omitted : {false, true}) {
      SCOPED_TRACE(Omitted);
      State.NativeMedFuncs.reset();
      State.FunctionLlvmModules.clear();
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Caller};
      if (!Omitted)
        Options.OnlyFunctionEntries.insert(Wrapper);
      State.PipeResult = Pipeline().run(State.Img, *State.LLVMCtx, Options);
      ASSERT_TRUE(State.PipeResult.Success) << State.PipeResult.Error;
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(NoOpt);
        const auto *Shown = State.ensureFunctionLlvmModule(Caller, NoOpt, true);
        ASSERT_TRUE(Shown) << State.LastError;
        ASSERT_TRUE(State.NativeMedFuncs);
        const auto &Calls = State.NativeMedFuncs->front().CallInfos;
        ASSERT_EQ(Calls.size(), 1U);
        ASSERT_EQ(Calls.front().Args.size(), 7U);
        EXPECT_TRUE(Calls.front().Args[4].isConst());
        EXPECT_EQ(Calls.front().Args[4].ConstVal, 0U);
        EXPECT_EQ(State.ensureFunctionLlvmModule(Caller, NoOpt, true), Shown);
      }
    }
  }
}

TEST_F(SessionLLVMTest, EmissionExceptionsKeepTheSessionAndReason) {
  State.Img.Arch = Arch::AArch64;
  auto Function = sourceFunction();
  MedOp Exit;
  Exit.Opcode = NdOp::INTRINSIC;
  Exit.addInput(MedVar::makeConst(static_cast<uint64_t>(Intrinsic::Eret),
                                  sizeof(uint16_t)));
  Function.Blocks.front().Ops.insert(Function.Blocks.front().Ops.begin(), Exit);
  State.PipeResult.MedFuncs.push_back(Function);
  std::shared_ptr<LLVMSourceMap> Sources;
  for (bool NoOpt : {false, true})
    for (bool OriginalImage : {false, true}) {
      State.clearError();
      EXPECT_NO_THROW(EXPECT_FALSE(
          State.emitNativeLlvm(NoOpt, nullptr, Sources, OriginalImage)));
      EXPECT_NE(State.LastError.find(kAArch64ExceptionReturnUnsupported),
                std::string::npos)
          << State.LastError;
      EXPECT_FALSE(Sources);
      EXPECT_TRUE(State.PipeRan);
      EXPECT_TRUE(State.PipeResult.Success);
      EXPECT_TRUE(State.FunctionLlvmModules.empty());
    }
  State.PipeResult.MedFuncs.front() = sourceFunction();
  State.NativeMedFuncs.reset();
  State.clearError();
  ASSERT_TRUE(State.emitNativeLlvm(true, nullptr, Sources)) << State.LastError;
  EXPECT_TRUE(State.LastError.empty());
}

TEST_F(SessionLLVMTest, SourceOnlyStackLimitSurvivesModulePreparation) {
  State.Img.Format = BinaryFormat::COFF;
  auto Function = sourceFunction();
  Function.HasUnresolvedSEHFrame = true;
  State.PipeResult.MedFuncs.push_back(Function);
  std::shared_ptr<LLVMSourceMap> Sources;
  for (bool NoOpt : {false, true})
    for (bool OriginalImage : {false, true}) {
      State.clearError();
      auto Module =
          State.emitNativeLlvm(NoOpt, nullptr, Sources, OriginalImage);
      ASSERT_TRUE(Module) << State.LastError;
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      const auto *Emitted = Module->getFunction(Function.Name);
      ASSERT_TRUE(Emitted);
      EXPECT_TRUE(
          Emitted->hasFnAttribute(windows_eh_md::UnresolvedSourceFrame));
      EXPECT_TRUE(State.LastError.empty());
    }
}

TEST_F(SessionLLVMTest, MissingIdentityNeverFallsBackToSymbolSpelling) {
  addFunction("sub_1000", std::nullopt);
  addFunction("sub_100", 0x2000);

  EXPECT_EQ(State.findNativeLlvmFunction(0x1000), nullptr);
  EXPECT_NE(State.LastError.find("not found at 0x1000"), std::string::npos);
}

TEST_F(SessionLLVMTest, MissingFunctionRetainsItsPipelineFailure) {
  addFunction("unrelated_source", 0x2000);
  PipelineFunctionAudit Rejected;
  Rejected.Entry = 0x1000;
  Rejected.Disposition = PipelineFunctionDisposition::RejectedIncomplete;
  Rejected.DecodeFailures = {0x1005};
  State.PipeResult.FunctionAudits.push_back(Rejected);
  const std::string Expected =
      "LLVM function not found at 0x1000 "
      "(rejected-incomplete; decode failure at 0x1005)";
  EXPECT_EQ(State.findNativeLlvmFunction(0x1000), nullptr);
  EXPECT_EQ(State.LastError, Expected);
  for (bool NoOpt : {false, true}) {
    State.clearError();
    EXPECT_EQ(State.ensureFunctionLlvmModule(0x1000, NoOpt), nullptr);
    EXPECT_EQ(State.LastError, Expected);
  }
  State.clearError();
  EXPECT_EQ(State.findNativeLlvmFunction(0x3000), nullptr);
  EXPECT_EQ(State.LastError, "LLVM function not found at 0x3000");
}

TEST_F(SessionLLVMTest, DuplicateOriginalAddressIsAmbiguous) {
  addFunction("first_source", 0x1000);
  addFunction("second_source", 0x1000);

  EXPECT_EQ(State.findNativeLlvmFunction(0x1000), nullptr);
  EXPECT_NE(State.LastError.find("ambiguous"), std::string::npos);
  EXPECT_NE(State.LastError.find("0x1000"), std::string::npos);
  EXPECT_NE(State.LastError.find("first_source"), std::string::npos);
  EXPECT_NE(State.LastError.find("second_source"), std::string::npos);
}

TEST_F(SessionLLVMTest, MalformedIdentityCannotBeHiddenByAnEarlierMatch) {
  addFunction("matching_source", 0x1000);
  auto *Malformed = addFunction("malformed_source", std::nullopt);
  Malformed->setMetadata(rewrite_source::FunctionAttachment,
                         llvm::MDNode::get(*State.LLVMCtx, {}));

  EXPECT_EQ(State.findNativeLlvmFunction(0x1000), nullptr);
  EXPECT_NE(State.LastError.find("invalid LLVM source identity"),
            std::string::npos);
  EXPECT_NE(State.LastError.find("malformed_source"), std::string::npos);
  EXPECT_NE(State.LastError.find("invalid operand count"), std::string::npos);
}

TEST_F(SessionLLVMTest, InvalidCandidateIsNeverCachedAndAnalysisRemainsUsable) {
  MedFunc Function = sourceFunction();
  // Duplicate block identity makes the real emitter produce a block with two
  // terminators. This tests cache publication, not a valid-binary lifting path.
  MedBlock Duplicate = Function.Blocks.front();
  Duplicate.StartAddr += 1;
  Function.Blocks.push_back(std::move(Duplicate));
  State.PipeResult.MedFuncs.push_back(std::move(Function));
  HighFunc High;
  High.Entry = 0x1000;
  High.Name = "available_high_ir";
  State.PipeResult.HighFuncs.push_back(std::move(High));
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "available_low_ir";
  State.PipeResult.LowFuncs.push_back(std::move(Low));
  const auto *Context = State.LLVMCtx.get();
  const auto *HighIR = State.findHighFunc(0x1000);
  const auto *LowIR = State.findLowFunc(0x1000);

  std::string Diagnostic;
  for (unsigned Attempt = 0; Attempt < 2; ++Attempt) {
    SCOPED_TRACE(Attempt);
    State.clearError();
    EXPECT_FALSE(State.ensureLlvmModule());
    EXPECT_EQ(State.PipeResult.LlvmModule, nullptr);
    EXPECT_NE(State.LastError.find("native LLVM verification failed"),
              std::string::npos);
    EXPECT_NE(State.LastError.find("Terminator found in the middle"),
              std::string::npos);
    if (Attempt == 0)
      Diagnostic = State.LastError;
    else
      EXPECT_EQ(State.LastError, Diagnostic);
    EXPECT_TRUE(State.ensurePipeline());
    EXPECT_TRUE(State.PipeResult.Success);
    EXPECT_EQ(State.LLVMCtx.get(), Context);
    EXPECT_EQ(State.findHighFunc(0x1000), HighIR);
    EXPECT_EQ(State.findLowFunc(0x1000), LowIR);
    EXPECT_TRUE(State.PipeResult.Error.empty());
  }

  State.PipeResult.MedFuncs.front().Blocks.pop_back();
  State.clearError();
  ASSERT_TRUE(State.ensureLlvmModule()) << State.LastError;
  ASSERT_NE(State.PipeResult.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*State.PipeResult.LlvmModule));
  EXPECT_TRUE(State.LastError.empty());
  const auto *Module = State.PipeResult.LlvmModule.get();
  EXPECT_NE(State.findNativeLlvmFunction(0x1000), nullptr);
  EXPECT_TRUE(State.ensureLlvmModule());
  EXPECT_EQ(State.PipeResult.LlvmModule.get(), Module);
}

TEST_F(SessionLLVMTest, SwitchingOptimizationPolicyRebuildsTheNativeModule) {
  State.PipeResult.MedFuncs.push_back(sourceFunction());

  ASSERT_TRUE(State.ensureLlvmModule(true)) << State.LastError;
  ASSERT_TRUE(State.LlvmModuleNoOpt);
  EXPECT_TRUE(*State.LlvmModuleNoOpt);
  State.PipeResult.LlvmModule->setModuleIdentifier("modified-no-opt-cache");
  ASSERT_TRUE(State.ensureLlvmModule(true)) << State.LastError;
  EXPECT_EQ(State.PipeResult.LlvmModule->getModuleIdentifier(),
            "modified-no-opt-cache");

  MedBlock Duplicate = State.PipeResult.MedFuncs.front().Blocks.front();
  Duplicate.StartAddr += 1;
  State.PipeResult.MedFuncs.front().Blocks.push_back(std::move(Duplicate));
  EXPECT_FALSE(State.ensureLlvmModule(false));
  ASSERT_NE(State.PipeResult.LlvmModule, nullptr);
  EXPECT_EQ(State.PipeResult.LlvmModule->getModuleIdentifier(),
            "modified-no-opt-cache");
  ASSERT_TRUE(State.LlvmModuleNoOpt);
  EXPECT_TRUE(*State.LlvmModuleNoOpt);
  State.clearError();
  EXPECT_TRUE(State.ensureLlvmModule(true));
  State.PipeResult.MedFuncs.front().Blocks.pop_back();

  ASSERT_TRUE(State.ensureLlvmModule(false)) << State.LastError;
  ASSERT_TRUE(State.LlvmModuleNoOpt);
  EXPECT_FALSE(*State.LlvmModuleNoOpt);
  EXPECT_NE(State.PipeResult.LlvmModule->getModuleIdentifier(),
            "modified-no-opt-cache");
  State.PipeResult.LlvmModule->setModuleIdentifier("modified-opt-cache");

  ASSERT_TRUE(State.ensureLlvmModule(true)) << State.LastError;
  ASSERT_TRUE(State.LlvmModuleNoOpt);
  EXPECT_TRUE(*State.LlvmModuleNoOpt);
  EXPECT_NE(State.PipeResult.LlvmModule->getModuleIdentifier(),
            "modified-opt-cache");
  EXPECT_FALSE(llvm::verifyModule(*State.PipeResult.LlvmModule));
}

TEST_F(SessionLLVMTest, RejectedEmissionDoesNotPublishAModule) {
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  State.Img.Segments.push_back(std::move(Text));
  MedFunc Function = sourceFunction();
  Function.ReturnType = NdType::makeInt(8);
  Function.Blocks.front().Ops.front().addInput(
      MedVar::makeConst(0x1080, 8, ConstantAddressProvenance::CodeAddress));
  State.PipeResult.MedFuncs.push_back(std::move(Function));

  EXPECT_FALSE(State.ensureLlvmModule());
  EXPECT_EQ(State.PipeResult.LlvmModule, nullptr);
  EXPECT_EQ(State.LastError, "native LLVM emission failed");
  EXPECT_TRUE(State.PipeResult.Success);
}

TEST_F(SessionLLVMTest, OneFunctionsModuleDoesNotEmitItsSiblings) {
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  State.Img.Segments.push_back(std::move(Text));
  State.PipeResult.MedFuncs.push_back(sourceFunction());
  // The emitter refuses this sibling: it returns a code address that no
  // lifted function owns.
  MedFunc Refused = sourceFunction();
  Refused.Entry = 0x1040;
  Refused.Name = "refused_sibling";
  Refused.ReturnType = NdType::makeInt(8);
  Refused.Blocks.front().StartAddr = Refused.Entry;
  Refused.Blocks.front().Ops.front().addInput(
      MedVar::makeConst(0x1080, 8, ConstantAddressProvenance::CodeAddress));
  State.PipeResult.MedFuncs.push_back(std::move(Refused));

  // The whole program refuses as a unit; one function's module does not.
  EXPECT_FALSE(State.ensureLlvmModule());
  State.clearError();
  const auto *Shown = State.ensureFunctionLlvmModule(0x1000, false);
  ASSERT_NE(Shown, nullptr) << State.LastError;
  EXPECT_FALSE(llvm::verifyModule(*Shown->Module));
  const auto *Body = State.findNativeLlvmFunction(*Shown->Module, 0x1000);
  ASSERT_NE(Body, nullptr) << State.LastError;
  EXPECT_FALSE(Body->isDeclaration());
  // The sibling is only declared, and showing the function again reuses
  // its module.
  EXPECT_EQ(State.findNativeLlvmFunction(*Shown->Module, 0x1040), nullptr);
  EXPECT_EQ(State.ensureFunctionLlvmModule(0x1000, false), Shown);

  // The refused function and an unknown one still fail clearly.
  State.clearError();
  EXPECT_EQ(State.ensureFunctionLlvmModule(0x1040, false), nullptr);
  EXPECT_EQ(State.LastError, "native LLVM emission failed");
  State.clearError();
  EXPECT_EQ(State.ensureFunctionLlvmModule(0x2000, false), nullptr);
  EXPECT_EQ(State.LastError, "LLVM function not found at 0x2000");

  // A new pipeline drops the modules before their context.
  State.clearPipeline();
  EXPECT_TRUE(State.FunctionLlvmModules.empty());
}

} // namespace
} // namespace neverd::sdk
