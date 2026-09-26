//===- LLVMCIntrinsicSemanticTests.cpp - Intrinsic C semantics ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Triple.h"

#include <vector>

namespace {
using namespace neverd;

std::string emit(llvm::Module &M) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.EmitIncludes = false;
  EXPECT_TRUE(LLVMCEmitter().emit(M, OS, Options));
  return Text;
}

struct StosFixture {
  llvm::LLVMContext Context;
  llvm::Module M{"rep-stos", Context};
  llvm::IRBuilder<> B{Context};
  llvm::Function *Fn;
  llvm::CallInst *Call;

  StosFixture(unsigned Bytes, X86RepStos::Direction Dir,
              llvm::StringRef Name = "fill", bool Live = false,
              bool BadConstraints = false, bool BadTemplate = false) {
    M.setDataLayout("e-p:64:64");
    const char Suffix = Bytes == 1   ? 'b'
                        : Bytes == 2 ? 'w'
                        : Bytes == 4 ? 'l'
                                     : 'q';
    std::string Rep = std::string("rep stos") + Suffix;
    std::string Asm =
        Dir == X86RepStos::Forward ? Rep
        : Dir == X86RepStos::Backward
            ? "std\n\t" + Rep + "\n\tcld"
            : "test $5,$5\n\tje 1f\n\tstd\n\t1:\n\t" + Rep + "\n\tcld";
    if (BadTemplate)
      Asm += "\n\tnop";
    const bool Dynamic = Dir == X86RepStos::Dynamic;
    std::string Constraints = std::string("={di},={cx},0,1,{ax},") +
                              (Dynamic ? "r," : "") +
                              "~{memory},~{dirflag},~{cc}";
    if (BadConstraints)
      Constraints += ",~{flags}";
    auto *Ret =
        llvm::StructType::get(Context, {B.getInt64Ty(), B.getInt64Ty()});
    std::vector<llvm::Type *> Types{B.getInt64Ty(), B.getInt64Ty(),
                                    B.getIntNTy(Bytes * 8)};
    if (Dynamic)
      Types.push_back(B.getInt64Ty());
    Fn = llvm::Function::Create(
        llvm::FunctionType::get(Live ? B.getInt64Ty() : B.getVoidTy(), Types,
                                false),
        llvm::GlobalValue::ExternalLinkage, Name, M);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Fn));
    auto *IA = llvm::InlineAsm::get(llvm::FunctionType::get(Ret, Types, false),
                                    Asm, Constraints, true);
    llvm::SmallVector<llvm::Value *, 4> Args;
    for (auto &Arg : Fn->args())
      Args.push_back(&Arg);
    Call = B.CreateCall(IA, Args);
    if (Live)
      B.CreateRet(B.CreateExtractValue(Call, 0));
    else
      B.CreateRetVoid();
  }
};

// Compile the emitted C itself. It checks both diagnostics and behavior, so a
// renderer that prints a plausible mnemonic but drops the memory effect fails.
void compileAndCheck(const std::string &Source, bool LLVMOnly = false,
                     llvm::ArrayRef<llvm::StringRef> ExpectedIR = {}) {
  auto Compiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(static_cast<bool>(Compiler)) << "clang is required";
  llvm::SmallString<128> Input, Output, Errors;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-intrinsic", "c", Input));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-intrinsic", "out", Output));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-intrinsic", "err", Errors));
  llvm::FileRemover RemoveInput(Input), RemoveOutput(Output),
      RemoveErrors(Errors);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(Input, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  llvm::SmallVector<llvm::StringRef, 16> Args{
      *Compiler,
      "-std=c11",
      "-O1",
      "-Werror=int-conversion",
      "-Werror=incompatible-pointer-types",
      "-Werror=uninitialized",
      Input,
      "-o",
      Output};
  if (LLVMOnly)
    Args.append({"-target", "x86_64-pc-windows-msvc", "-S", "-emit-llvm"});
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, Errors.str()};
  std::string Error;
  int Status = llvm::sys::ExecuteAndWait(*Compiler, Args, std::nullopt,
                                         Redirects, 30, 0, &Error);
  auto Log = llvm::MemoryBuffer::getFile(Errors);
  ASSERT_EQ(Status, 0) << Error << (Log ? (*Log)->getBuffer().str() : "")
                       << Source;
  if (LLVMOnly) {
    auto IR = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(static_cast<bool>(IR));
    if (ExpectedIR.empty()) {
      EXPECT_TRUE((*IR)->getBuffer().contains("call ptr @llvm.localaddress()"))
          << (*IR)->getBuffer().str();
    } else {
      for (llvm::StringRef Token : ExpectedIR)
        EXPECT_TRUE((*IR)->getBuffer().contains(Token))
            << (*IR)->getBuffer().str();
      EXPECT_TRUE((*IR)->getBuffer().contains("br i1"))
          << (*IR)->getBuffer().str();
    }
  } else {
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}, std::nullopt, {}, 30,
                                        0, &Error),
              0)
        << Error << Source;
  }
}

TEST(LLVMCIntrinsicSemantics, RepStosFrameBufferSharesBackingWithIndexedLoads) {
  std::string Source = "#include <stdint.h>\n";
  for (bool Backward : {false, true}) {
    llvm::LLVMContext C;
    llvm::Module M("frame-stos", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<llvm::NoFolder> B(C);
    const std::string Name = Backward ? "frame_fill_backward" : "frame_fill";
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(),
                                {B.getInt64Ty(), B.getInt64Ty()}, false),
        llvm::GlobalValue::ExternalLinkage, Name, M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", F));
    auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 160),
                                 nullptr, "frame");
    auto Addr = [&](uint64_t Offset) {
      return B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(Offset));
    };
    B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(144), "frame_end");
    B.CreateStore(B.getInt8(0xCC), Addr(79));
    B.CreateStore(B.getInt8(0xDD), Addr(144));
    B.CreateStore(B.getInt8(0), Addr(80));
    auto *RetTy = llvm::StructType::get(C, {B.getInt64Ty(), B.getInt64Ty()});
    auto *AsmTy = llvm::FunctionType::get(
        RetTy, {B.getInt64Ty(), B.getInt64Ty(), B.getInt8Ty()}, false);
    auto *IA = llvm::InlineAsm::get(
        AsmTy, Backward ? "std\n\trep stosb\n\tcld" : "rep stosb",
        "={di},={cx},0,1,{ax},~{memory},~{dirflag},~{cc}", true);
    B.CreateCall(IA,
                 {B.CreatePtrToInt(Addr(Backward ? 143 : 80), B.getInt64Ty()),
                  F->getArg(1), B.getInt8(0x5A)});
    auto *Indexed = B.CreateGEP(B.getInt8Ty(), Frame,
                                B.CreateAdd(F->getArg(0), B.getInt64(80)));
    auto Load = [&](llvm::Value *Ptr) {
      return B.CreateZExt(B.CreateLoad(B.getInt8Ty(), Ptr), B.getInt32Ty());
    };
    auto *Result =
        B.CreateOr(Load(Indexed), B.CreateShl(Load(Addr(80)), B.getInt32(8)));
    Result = B.CreateOr(Result, B.CreateShl(Load(Addr(79)), B.getInt32(16)));
    Result = B.CreateOr(Result, B.CreateShl(Load(Addr(144)), B.getInt32(24)));
    B.CreateRet(Result);
    const std::string Text = emit(M);
    EXPECT_EQ(Text.find("&var_"), std::string::npos) << Text;
    Source += Text;
  }
  Source += "int main(void) { for(uint64_t i=0;i<64;++i) { "
            "if(frame_fill(i,64)!=0xDDCC5A5Au) return 1; "
            "if(frame_fill_backward(i,64)!=0xDDCC5A5Au) return 2; "
            "} return 0; }\n";
  compileAndCheck(Source);
}

TEST(LLVMCIntrinsicSemantics, RepStosForwardBackwardDynamicAndZeroCount) {
  std::string Source = "#include <stdint.h>\n";
  for (unsigned Bytes : {1u, 2u, 4u, 8u}) {
    for (auto Dir :
         {X86RepStos::Forward, X86RepStos::Backward, X86RepStos::Dynamic}) {
      const std::string Name =
          "fill" + std::to_string(Bytes) + "_" + std::to_string(Dir);
      StosFixture F(Bytes, Dir, Name);
      ASSERT_TRUE(classifyX86RepStos(Arch::X64, *F.Call));
      Source += emit(F.M);
    }
  }
  Source += "int main(void) { unsigned char data[80];\n";
  for (unsigned Bytes : {1u, 2u, 4u, 8u}) {
    for (auto Dir :
         {X86RepStos::Forward, X86RepStos::Backward, X86RepStos::Dynamic}) {
      for (bool Backward : {false, true}) {
        if (Dir != X86RepStos::Dynamic &&
            Backward != (Dir == X86RepStos::Backward))
          continue;
        const std::string Name =
            "fill" + std::to_string(Bytes) + "_" + std::to_string(Dir);
        const std::string Df =
            Dir == X86RepStos::Dynamic ? (Backward ? ",7" : ",0") : "";
        const unsigned Start = 8 + (Backward ? 3 * Bytes : 0);
        Source += "for (unsigned i=0;i<80;++i) data[i]=0xCC;\n";
        Source += Name + "((uintptr_t)&data[" + std::to_string(Start) +
                  "],4,(uint" + std::to_string(Bytes * 8) +
                  "_t)0x8877665544332211ULL" + Df + ");\n";
        Source += "for (unsigned i=0;i<80;++i) { unsigned char expected=0xCC; "
                  "if(i>=8 && i<" +
                  std::to_string(8 + 4 * Bytes) +
                  ") "
                  "expected=(unsigned char)(0x8877665544332211ULL >> (((i-8)%" +
                  std::to_string(Bytes) +
                  ")*8)); if(data[i]!=expected)return 1; }\n";
        Source += Name + "(0,0,0" + Df + ");\n";
      }
    }
  }
  Source += "return 0;}\n";
  compileAndCheck(Source);
}

TEST(LLVMCIntrinsicSemantics, RepStosRejectsUnprovedContracts) {
  for (unsigned Variant = 0; Variant < 3; ++Variant) {
    StosFixture F(1, X86RepStos::Dynamic, "bad_fill", Variant == 0,
                  Variant == 1, Variant == 2);
    EXPECT_FALSE(classifyX86RepStos(Arch::X64, *F.Call));
    EXPECT_DEATH((void)emit(F.M),
                 "unsupported REP STOS inline assembly contract");
  }
}

TEST(LLVMCIntrinsicSemantics,
     LocalAddressKeepsTargetIntrinsicAndInitializesResult) {
  llvm::LLVMContext C;
  llvm::Module M("localaddress", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *Fn =
      llvm::Function::Create(llvm::FunctionType::get(B.getVoidTy(), false),
                             llvm::GlobalValue::ExternalLinkage, "parent", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto *Address = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
      &M, llvm::Intrinsic::localaddress));
  auto Consume =
      M.getOrInsertFunction("use_parent", B.getVoidTy(), B.getPtrTy());
  B.CreateCall(Consume, {Address});
  B.CreateRetVoid();
  const auto Text = emit(M);
  EXPECT_NE(Text.find("__asm__(\"llvm.localaddress\")"), std::string::npos)
      << Text;
  compileAndCheck(Text, true);
}

TEST(LLVMCIntrinsicSemantics, IntegerAddressAdditionUsesIntegerArithmetic) {
  llvm::LLVMContext C;
  llvm::Module M("address-add", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy(), B.getInt64Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "add_address", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto *Address = B.CreatePtrToInt(Fn->getArg(0), B.getInt64Ty());
  auto *Added = B.CreateAdd(Address, Fn->getArg(1));
  // Two uses force the addition to remain a statement in the C output.
  auto Sink =
      M.getOrInsertFunction("use_integer", B.getVoidTy(), B.getInt64Ty());
  B.CreateCall(Sink, {Added});
  B.CreateRet(Added);
  const auto Text = emit(M);
  compileAndCheck("#include <stdint.h>\n" + Text +
                  "void use_integer(uint64_t x) {(void)x;}\n"
                  "int main(void) {char b[8]; return add_address(b, "
                  "UINT64_MAX) != (uintptr_t)b-1;}\n");
}

TEST(LLVMCIntrinsicSemantics, ConstantRotateGuardsUseActualIntegerWidths) {
  llvm::LLVMContext C;
  llvm::Module M("rotate-guards", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<llvm::NoFolder> B(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt64Ty(), {B.getInt64Ty()}, false),
      llvm::GlobalValue::ExternalLinkage, "rotate_guard", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto Shift = [&](unsigned Count, bool Left) {
    auto *Home = B.CreateAlloca(B.getInt64Ty());
    B.CreateStore(B.getInt64(Count), Home);
    auto *N = B.CreateLoad(B.getInt64Ty(), Home);
    auto *Masked = B.CreateURem(N, B.getInt64(64));
    auto *Value = Left ? B.CreateShl(Fn->getArg(0), Masked)
                       : B.CreateLShr(Fn->getArg(0), Masked);
    return B.CreateSelect(B.CreateICmpULT(N, B.getInt64(64)), Value,
                          B.getInt64(0));
  };
  auto *Rotated = B.CreateOr(Shift(16, true), Shift(48, false));
  // i8 255 is negative only for signed comparison; zext must retain 255.
  auto *Unsigned = B.CreateICmpULT(B.getInt8(255), B.getInt8(1));
  auto *Signed = B.CreateICmpSLT(B.getInt8(255), B.getInt8(1));
  auto *Extended = B.CreateICmpEQ(B.CreateZExt(B.getInt8(255), B.getInt64Ty()),
                                  B.getInt64(255));
  auto *Wrapped =
      B.CreateICmpEQ(B.CreateAdd(B.getInt8(255), B.getInt8(1)), B.getInt8(0));
  auto *Correct = B.CreateAnd(B.CreateAnd(B.CreateNot(Unsigned), Signed),
                              B.CreateAnd(Extended, Wrapped));
  B.CreateRet(B.CreateSelect(Correct, Rotated, B.getInt64(0)));
  const auto Text = emit(M);
  EXPECT_EQ(Text.find(" % "), std::string::npos) << Text;
  EXPECT_EQ(Text.find(" ? "), std::string::npos) << Text;
  compileAndCheck(
      "#include <stdint.h>\n" + Text +
      "int main(void) { uint64_t a[]={0,1,0x8000000000000001ULL,UINT64_MAX};"
      "for(unsigned i=0;i<4;++i)if(rotate_guard(a[i])!=((a[i]<<16)|(a[i]>>48)))"
      "return 1; return 0;}\n");
}

TEST(LLVMCIntrinsicSemantics, ConstantSelectPreservesCallAndVolatileProducer) {
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    llvm::LLVMContext C;
    llvm::Module M("select-effects", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<llvm::NoFolder> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy(), B.getInt1Ty()},
                                false),
        llvm::GlobalValue::ExternalLinkage, "select_effects", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
    auto Callee = M.getOrInsertFunction("tick", B.getInt64Ty());
    auto *Called = B.CreateCall(Callee);
    auto *Loaded = B.CreateLoad(B.getInt64Ty(), Fn->getArg(0));
    Loaded->setVolatile(Mode != 1);
    if (Mode != 0)
      Loaded->setAtomic(Mode == 1
                            ? llvm::AtomicOrdering::Acquire
                            : llvm::AtomicOrdering::SequentiallyConsistent);
    auto *Chosen = B.CreateSelect(B.CreateICmpULT(B.getInt8(255), B.getInt8(1)),
                                  Called, Loaded);
    B.CreateRet(B.CreateSelect(Fn->getArg(1), Chosen, B.getInt64(9)));
    const auto Text = emit(M);
    EXPECT_NE(Text.find("tick()"), std::string::npos) << Text;
    if (Mode != 1)
      EXPECT_NE(Text.find("volatile"), std::string::npos) << Text;
    if (Mode != 0) {
      EXPECT_NE(Text.find("__atomic_load("), std::string::npos) << Text;
      EXPECT_NE(Text.find(Mode == 1 ? "__ATOMIC_ACQUIRE" : "__ATOMIC_SEQ_CST"),
                std::string::npos)
          << Text;
    }
    EXPECT_NE(Text.find(" ? "), std::string::npos) << Text;
    compileAndCheck("#include <stdint.h>\n#include <stdbool.h>\n" + Text +
                    "static int calls; uint64_t tick(void) {++calls;return 3;}"
                    "int main(void) { uint64_t x=17; "
                    "if(select_effects(&x,1)!=17 || calls!=1)"
                    "return 1; if(select_effects(&x,0)!=9 || calls!=2)return "
                    "2;return 0;}\n");
  }
}

TEST(LLVMCIntrinsicSemantics, VolatileAndAtomicPointerLoadsQualifyTheSlot) {
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    llvm::LLVMContext C;
    llvm::Module M("pointer-load", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getPtrTy(), {B.getPtrTy()}, false),
        llvm::GlobalValue::ExternalLinkage, "read_pointer", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
    auto *Loaded = B.CreateLoad(B.getPtrTy(), Fn->getArg(0));
    Loaded->setVolatile(Mode != 1);
    if (Mode != 0)
      Loaded->setAtomic(Mode == 1
                            ? llvm::AtomicOrdering::Acquire
                            : llvm::AtomicOrdering::SequentiallyConsistent);
    B.CreateRet(Loaded);
    const auto Text = emit(M);
    if (Mode != 1)
      EXPECT_NE(Text.find("void* volatile*"), std::string::npos) << Text;
    compileAndCheck(
        "#include <stdint.h>\n" + Text +
        "int main(void) {int x=7;void *p=&x;return read_pointer(&p)!=&x;}\n");
  }
}

TEST(LLVMCIntrinsicSemantics, UnusedObservableLoadKeepsConditionalBlock) {
  for (bool Atomic : {false, true}) {
    SCOPED_TRACE(Atomic);
    llvm::LLVMContext C;
    llvm::Module M("unused-observable-load", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), {B.getPtrTy(), B.getInt1Ty()},
                                false),
        llvm::GlobalValue::ExternalLinkage, "conditional_read", M);
    auto *Entry = llvm::BasicBlock::Create(C, "entry", Fn);
    auto *Probe = llvm::BasicBlock::Create(C, "probe", Fn);
    auto *Join = llvm::BasicBlock::Create(C, "join", Fn);
    B.SetInsertPoint(Entry);
    B.CreateCondBr(Fn->getArg(1), Probe, Join);
    B.SetInsertPoint(Probe);
    auto *Loaded = B.CreateLoad(B.getInt32Ty(), Fn->getArg(0), "observed");
    if (Atomic)
      Loaded->setAtomic(llvm::AtomicOrdering::Acquire);
    else
      Loaded->setVolatile(true);
    B.CreateBr(Join);
    B.SetInsertPoint(Join);
    B.CreateRet(B.getInt32(7));
    const auto Text = "#include <stdint.h>\n" + emit(M);
    // Windows C may strengthen a volatile read to an atomic acquire read.
    compileAndCheck(
        Text, true,
        Atomic ? std::vector<llvm::StringRef>{"load atomic", " acquire"}
               : std::vector<llvm::StringRef>{"volatile i32"});
    compileAndCheck(
        Text + "int main(void){uint32_t x=1;return "
               "conditional_read(0,0)!=7 || conditional_read(&x,1)!=7;}\n");
  }
}

struct ObservableFieldDebug final : NullDebugContext {
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    if (Address != 0x1000)
      return std::nullopt;
    auto Record = NdType::makeNamedRecord("ObservedRecord", 8);
    Record->FieldDisplayNames = {"bits"};
    Record->FieldDisplayOffsets = {4};
    Record->FieldDisplayTypes = {NdType::makeInt(4, false)};
    FunctionSym F;
    F.Name = "read_then_clear";
    F.Addr = Address;
    F.Params = {{"this", NdType::makePtr(Record)}};
    return F;
  }
  bool hasInfo() const override { return true; }
};

TEST(LLVMCIntrinsicSemantics, ObservableFieldCastUsesCapturedValue) {
  for (bool Atomic : {false, true}) {
    SCOPED_TRACE(Atomic);
    llvm::LLVMContext C;
    llvm::Module M("observable-field", C);
    M.setDataLayout("e-p:64:64");
    llvm::IRBuilder<llvm::NoFolder> B(C);
    auto *Fn = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy()}, false),
        llvm::GlobalValue::ExternalLinkage, "read_then_clear", M);
    rewrite_source::setOriginalVA(*Fn, 0x1000);
    B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
    auto *Ptr = B.CreateGEP(B.getInt8Ty(), Fn->getArg(0), B.getInt64(4));
    auto *Loaded = B.CreateLoad(B.getInt32Ty(), Ptr, "observed_field");
    if (Atomic)
      Loaded->setAtomic(llvm::AtomicOrdering::Acquire);
    else
      Loaded->setVolatile(true);
    B.CreateStore(B.getInt32(0), Ptr);
    B.CreateRet(B.CreateSExt(Loaded, B.getInt64Ty()));
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    CEmitterOptions Options;
    Options.EmitIncludes = false;
    ObservableFieldDebug Debug;
    ASSERT_TRUE(LLVMCEmitter().emit(M, OS, Options, &Debug));
    compileAndCheck(
        "#include <stdint.h>\n"
        "typedef struct {uint32_t pad,bits;} ObservedRecord;\n" +
        Text +
        "int main(void){ObservedRecord r={0,0x80000001u};return "
        "read_then_clear(&r)!=UINT64_C(0xffffffff80000001) || r.bits!=0;}\n");
  }
}

TEST(LLVMCIntrinsicSemantics, ObservableReadonlyImageLoadKeepsDeclaration) {
  for (bool Only : {false, true}) {
    for (bool Atomic : {false, true}) {
      SCOPED_TRACE(Only);
      SCOPED_TRACE(Atomic);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      Image.Base = 0x1000;
      Segment Data;
      Data.VA = 0x2000;
      Data.Size = 4;
      Data.FileSz = 4;
      Data.Flags = SegmentFlags::Readable;
      Data.Data = {17, 0, 0, 0};
      Image.Segments.push_back(Data);
      llvm::LLVMContext C;
      llvm::Module M("readonly-observable", C);
      M.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> B(C);
      auto *Global = new llvm::GlobalVariable(
          M, B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage, nullptr,
          "__nd_data_2000");
      auto *Fn = llvm::Function::Create(
          llvm::FunctionType::get(B.getInt32Ty(), false),
          llvm::GlobalValue::ExternalLinkage, "read_image", M);
      B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
      auto *Loaded = B.CreateLoad(B.getInt32Ty(), Global);
      if (Atomic)
        Loaded->setAtomic(llvm::AtomicOrdering::Acquire);
      else
        Loaded->setVolatile(true);
      B.CreateRet(Loaded);
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      CEmitterOptions Options;
      Options.EmitIncludes = false;
      ASSERT_TRUE(LLVMCEmitter().emit(M, OS, Options, nullptr, &Image,
                                      Only ? Fn : nullptr));
      EXPECT_NE(Text.find("extern uint32_t g_2000;"), std::string::npos)
          << Text;
      // The snapshot says 17, but the retained read must observe the actual
      // C object's value rather than silently substituting that snapshot.
      compileAndCheck(
          "#include <stdint.h>\n" + Text +
          "uint32_t g_2000=29;int main(void){return read_image()!=29;}\n");
    }
  }
}

TEST(LLVMCIntrinsicSemantics, X87FpremKeepsTenByteOperandsAndC2) {
  llvm::LLVMContext C;
  llvm::Module M("x87-fprem-c2", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *I80 = B.getIntNTy(80);
  auto *F80 = llvm::Type::getX86_FP80Ty(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt32Ty(),
                              {B.getPtrTy(), B.getPtrTy(), B.getPtrTy(),
                               B.getPtrTy()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "fprem_c2", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto Arg = Fn->arg_begin();
  llvm::Value *Large = &*Arg++;
  llvm::Value *Small = &*Arg++;
  llvm::Value *Remainder = &*Arg++;
  llvm::Value *StatusOut = &*Arg;
  auto *Init = llvm::InlineAsm::get(
      llvm::FunctionType::get(B.getVoidTy(), {}, false), "fninit",
      "~{memory}", true);
  B.CreateCall(Init);
  auto *LargeBits = B.CreateLoad(I80, Large, "large_bits");
  LargeBits->setVolatile(true);
  auto *SmallBits = B.CreateLoad(I80, Small, "small_bits");
  SmallBits->setVolatile(true);
  auto *Fprem = llvm::InlineAsm::get(
      llvm::FunctionType::get(F80, {F80, F80}, false), "fprem",
      "=&{st},0,{st(1)},~{dirflag},~{fpsr},~{flags}", true);
  auto *Reduced = B.CreateCall(
      Fprem, {B.CreateBitCast(LargeBits, F80),
              B.CreateBitCast(SmallBits, F80)},
      "partial_remainder");
  // A pure bitcast between FPREM and FNSTSW can make C compilation spill and
  // pop the x87 stack.  The status must be captured in the same asm as FPREM.
  auto *RemainderBits = B.CreateBitCast(Reduced, I80);
  auto *Fnstsw = llvm::InlineAsm::get(
      llvm::FunctionType::get(B.getInt16Ty(), {}, false), "fnstsw $0",
      "={ax},~{dirflag},~{fpsr},~{flags}", true);
  auto *Status = B.CreateCall(Fnstsw, {}, "x87_status");
  B.CreateStore(Status, StatusOut)->setVolatile(true);
  B.CreateStore(RemainderBits, Remainder)->setVolatile(true);
  auto *C2 = B.CreateAnd(Status, B.getInt16(0x0400));
  B.CreateRet(B.CreateSelect(B.CreateICmpNE(C2, B.getInt16(0)),
                             B.getInt32(0), B.getInt32(1)));

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("__uint128_t"), std::string::npos) << Text;
  EXPECT_NE(Text.find("long double"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__builtin_memcpy"), std::string::npos) << Text;
  EXPECT_NE(Text.find("fprem\\n\\tfnstsw %%ax"), std::string::npos)
      << Text;
  EXPECT_NE(Text.find("fnstsw %%ax"), std::string::npos) << Text;
  EXPECT_NE(Text.find("1024"), std::string::npos) << Text;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck(
      "#include <stdint.h>\n" + Text +
      "int main(void) {\n"
      "  unsigned char large[10] = {1,0,0,0,0,0,0,0x80,0xfe,0x7f};\n"
      "  unsigned char small[10] = {3,0,0,0,0,0,0,0x80,0xbe,0xff};\n"
      "  unsigned char remainder[10] = {0};\n"
      "  uint16_t status = 0;\n"
      "  int result = fprem_c2(large, small, remainder, &status);\n"
      "  return result || (status & 0x3c00) != 0x3400;\n"
      "}\n");
#endif
}

TEST(LLVMCIntrinsicSemantics, X87FusedFpremStatusKeepsTop) {
  llvm::LLVMContext C;
  llvm::Module M("x87-fused-fprem-status", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *I80 = B.getIntNTy(80);
  auto *F80 = llvm::Type::getX86_FP80Ty(C);
  auto *Pair = llvm::StructType::create(C, "struct.neverd.x87.fprem_result");
  Pair->setBody({F80, B.getInt16Ty()});
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(),
                              {B.getPtrTy(), B.getPtrTy(), B.getPtrTy()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "fused_fprem", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto Arg = Fn->arg_begin();
  llvm::Value *Large = &*Arg++;
  llvm::Value *Small = &*Arg++;
  llvm::Value *StatusOut = &*Arg;
  auto *Init = llvm::InlineAsm::get(
      llvm::FunctionType::get(B.getVoidTy(), {}, false), "fninit",
      "~{memory}", true);
  B.CreateCall(Init);
  auto *LargeBits = B.CreateLoad(I80, Large, "large_bits");
  LargeBits->setVolatile(true);
  auto *SmallBits = B.CreateLoad(I80, Small, "small_bits");
  SmallBits->setVolatile(true);
  auto *Fprem = llvm::InlineAsm::get(
      llvm::FunctionType::get(Pair, {F80, F80}, false),
      "fprem\n\tfnstsw $1",
      "=&{st},={ax},0,{st(1)},~{dirflag},~{fpsr},~{flags}", true);
  auto *Reduced = B.CreateCall(
      Fprem, {B.CreateBitCast(LargeBits, F80),
              B.CreateBitCast(SmallBits, F80)},
      "partial_remainder_and_status");
  auto *RemainderBits =
      B.CreateBitCast(B.CreateExtractValue(Reduced, 0), I80);
  B.CreateStore(B.CreateExtractValue(Reduced, 1), StatusOut)
      ->setVolatile(true);
  (void)RemainderBits;
  B.CreateRetVoid();

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("fprem\\n\\tfnstsw %%ax"), std::string::npos)
      << Text;
  EXPECT_NE(Text.find("field_0"), std::string::npos) << Text;
  EXPECT_NE(Text.find("field_1"), std::string::npos) << Text;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck(
      "#include <stdint.h>\n" + Text +
      "int main(void) {\n"
      "  unsigned char large[10] = {1,0,0,0,0,0,0,0x80,0xfe,0x7f};\n"
      "  unsigned char small[10] = {3,0,0,0,0,0,0,0x80,0xbe,0xff};\n"
      "  uint16_t status = 0;\n"
      "  fused_fprem(large, small, &status);\n"
      "  return (status & 0x3c00) != 0x3400;\n"
      "}\n");
#endif
}

TEST(LLVMCIntrinsicSemantics, LinuxX64SyscallUsesRegisterABI) {
  llvm::LLVMContext C;
  llvm::Module M("linux-x64-syscall", C);
  M.setDataLayout("e-p:64:64");
  M.setTargetTriple(llvm::Triple("x86_64-unknown-linux-gnu"));
  llvm::IRBuilder<> B(C);
  auto *I64 = B.getInt64Ty();
  auto *Pair = llvm::StructType::create(C, "neverd.x64.syscall_result");
  Pair->setBody({I64, I64});
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(I64, {B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "neverd_test_getpid", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto *Syscall = llvm::InlineAsm::get(
      llvm::FunctionType::get(Pair,
                              {I64, I64, I64, I64, I64, I64, I64}, false),
      "syscall",
      "={ax},={r11},0,{di},{si},{dx},{r10},{r8},{r9},~{rcx},~{memory},"
      "~{dirflag},~{fpsr},~{flags}",
      true);
  auto *PairValue = B.CreateCall(
      Syscall, {B.getInt64(39), B.getInt64(0), B.getInt64(0), B.getInt64(0),
                B.getInt64(0), B.getInt64(0), B.getInt64(0)});
  B.CreateStore(B.CreateExtractValue(PairValue, 1), &*Fn->arg_begin());
  B.CreateRet(B.CreateExtractValue(PairValue, 0));

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("__asm__(\"r10\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__(\"r8\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__(\"r9\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__(\"r11\")"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__asm__ volatile(\"syscall\""),
            std::string::npos)
      << Text;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck(
      "#include <stdint.h>\n#include <unistd.h>\n" + Text +
      "int main(void) {\n"
      "  uint64_t flags = 0;\n"
      "  uint64_t pid = neverd_test_getpid(&flags);\n"
      "  return pid == (uint64_t)getpid() && (flags & 2) ? 0 : 1;\n"
      "}\n");
#endif
}

TEST(LLVMCIntrinsicSemantics, X87I80BitcastMaterializesExpressionSource) {
  llvm::LLVMContext C;
  llvm::Module M("x87-i80-bitcast-expression", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<llvm::NoFolder> B(C);
  auto *I80 = B.getIntNTy(80);
  auto *F80 = llvm::Type::getX86_FP80Ty(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(I80, {I80}, false),
      llvm::GlobalValue::ExternalLinkage, "x87_bits_roundtrip", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto *Value = B.CreateAdd(&*Fn->arg_begin(), llvm::ConstantInt::get(I80, 1));
  B.CreateRet(B.CreateBitCast(B.CreateBitCast(Value, F80), I80));

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("__uint128_t"), std::string::npos) << Text;
  EXPECT_NE(Text.find("__builtin_memcpy"), std::string::npos) << Text;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck(
      "#include <stdint.h>\n" + Text +
      "int main(void) {\n"
      "  __uint128_t bits = (((__uint128_t)0x7ffeULL << 64) | "
      "0x8000000000000001ULL);\n"
      "  return x87_bits_roundtrip(bits) == bits + 1 ? 0 : 1;\n"
      "}\n");
#endif
}

TEST(LLVMCIntrinsicSemantics, VolatileI80PointerCopyKeepsTenByteAccesses) {
  llvm::LLVMContext C;
  llvm::Module M("volatile-i80-pointer-copy", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy(), B.getPtrTy()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "copy_volatile_i80", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto Arg = Fn->arg_begin();
  auto *Bits = B.CreateLoad(B.getIntNTy(80), &*Arg++, "bits");
  Bits->setVolatile(true);
  B.CreateStore(Bits, &*Arg)->setVolatile(true);
  B.CreateRetVoid();

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("((const volatile uint8_t*)"), std::string::npos) << Text;
  EXPECT_NE(Text.find("((volatile uint8_t*)"), std::string::npos) << Text;
  EXPECT_NE(Text.find("x87_byte"), std::string::npos) << Text;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck("#include <stdint.h>\n" + Text +
                  "int main(void) {\n"
                  "  uint8_t source[16] = {1,2,3,4,5,6,7,8,9,10,0xee};\n"
                  "  uint8_t destination[16] = {0};\n"
                  "  destination[10] = 0x77;\n"
                  "  copy_volatile_i80(source, destination);\n"
                  "  for (unsigned i = 0; i < 10; ++i)\n"
                  "    if (destination[i] != source[i]) return 1;\n"
                  "  return source[10] != 0xee || destination[10] != 0x77;\n"
                  "}\n");
#endif
}

TEST(LLVMCIntrinsicSemantics, X87OperandsShareOverlappingImageByteArray) {
  llvm::LLVMContext C;
  llvm::Module M("x87-overlapping-image-data", C);
  M.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(C);
  auto *Bytes = llvm::ArrayType::get(B.getInt8Ty(), 32);
  auto *ImageData = new llvm::GlobalVariable(
      M, Bytes, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantAggregateZero::get(Bytes), "__nd_data_402000.data");
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy(), B.getPtrTy()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "copy_x87_operands", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Fn));
  auto At = [&](uint64_t Offset) {
    return B.CreateGEP(B.getInt8Ty(), ImageData, B.getInt64(Offset));
  };
  B.CreateStore(B.getInt64(0x8000000000000001ULL), At(0))->setVolatile(true);
  B.CreateStore(B.getInt16(0x7ffe), At(8))->setVolatile(true);
  B.CreateStore(B.getInt64(0x8000000000000003ULL), At(10))->setVolatile(true);
  B.CreateStore(B.getInt16(0xffbe), At(18))->setVolatile(true);
  auto *Large = B.CreateLoad(B.getIntNTy(80), At(0), "large_bits");
  Large->setVolatile(true);
  auto *Small = B.CreateLoad(B.getIntNTy(80), At(10), "small_bits");
  Small->setVolatile(true);
  auto Arg = Fn->arg_begin();
  B.CreateStore(Large, &*Arg++)->setVolatile(true);
  B.CreateStore(Small, &*Arg)->setVolatile(true);
  B.CreateStore(Small, At(20))->setVolatile(true);
  B.CreateStore(B.getInt16(0x0400), At(30))->setVolatile(true);
  B.CreateRetVoid();

  const std::string Text = emit(M);
  EXPECT_NE(Text.find("uint8_t g_402000[32] = {0};"), std::string::npos)
      << Text;
  EXPECT_EQ(Text.find("g_402008"), std::string::npos) << Text;
  EXPECT_EQ(Text.find("g_402012"), std::string::npos) << Text;
  EXPECT_NE(Text.find("g_402000 + 10"), std::string::npos) << Text;
  EXPECT_NE(Text.find("g_402000 + 30"), std::string::npos) << Text;
  std::string OnlyText;
  llvm::raw_string_ostream OnlyOS(OnlyText);
  CEmitterOptions OnlyOptions;
  OnlyOptions.TheArch = Arch::X64;
  OnlyOptions.EmitIncludes = false;
  EXPECT_TRUE(LLVMCEmitter().emit(M, OnlyOS, OnlyOptions, nullptr, nullptr, Fn));
  EXPECT_NE(OnlyText.find("uint8_t g_402000[32] = {0};"),
            std::string::npos)
      << OnlyText;
  EXPECT_EQ(OnlyText.find("g_402008"), std::string::npos) << OnlyText;
#if defined(__x86_64__) && defined(__linux__)
  compileAndCheck(
      "#include <stdint.h>\n" + Text +
      "int main(void) {\n"
      "  unsigned char large[10] = {0}, small[10] = {0};\n"
      "  const unsigned char want_large[10] = "
      "{1,0,0,0,0,0,0,0x80,0xfe,0x7f};\n"
      "  const unsigned char want_small[10] = "
      "{3,0,0,0,0,0,0,0x80,0xbe,0xff};\n"
      "  copy_x87_operands(large, small);\n"
      "  for (unsigned i = 0; i < 10; ++i) {\n"
      "    if (large[i] != want_large[i] || "
      "g_402000[i] != want_large[i]) return 1;\n"
      "    if (small[i] != want_small[i] || "
      "g_402000[10 + i] != want_small[i] || "
      "g_402000[20 + i] != want_small[i]) return 2;\n"
      "  }\n"
      "  return g_402000[30] == 0 && g_402000[31] == 4 ? 0 : 3;\n"
      "}\n");
#endif
}

} // namespace
