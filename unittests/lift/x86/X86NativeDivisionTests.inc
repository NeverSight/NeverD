//===- X86NativeDivisionTests.inc - Native division C semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify native result pairs, ignored results, and divide errors in LLVM-C.
//===----------------------------------------------------------------------===//

TEST(LLVMCValues, X86DivideIntrinsicRejectsIncompleteEffectAndTypeContracts) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    llvm::LLVMContext Context;
    llvm::Module Module("invalid-division-intrinsic", Context);
    llvm::IRBuilder<> B(Context);
    auto *Word = B.getIntNTy(Mutation == 0 ? 32 : 64);
    auto *Pair = llvm::StructType::get(Context, {Word, Word});
    auto *Native = llvm::Function::Create(
        llvm::FunctionType::get(Pair, {Word, Word, Word}, false),
        llvm::GlobalValue::ExternalLinkage, "llvm.x86.udivrem64", Module);
    Native->addFnAttr(llvm::Attribute::NoUnwind);
    Native->addFnAttr(llvm::Attribute::Convergent);
    Native->addFnAttr(llvm::Attribute::NoDuplicate);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "invoke", Module);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Call = B.CreateCall(Native, {llvm::ConstantInt::get(Word, 0),
                                       llvm::ConstantInt::get(Word, 0),
                                       llvm::ConstantInt::get(Word, 1)});
    B.CreateRetVoid();
    if (Mutation == 1)
      Native->removeFnAttr(llvm::Attribute::Convergent);
    if (Mutation == 2)
      Native->removeFnAttr(llvm::Attribute::NoDuplicate);
    if (Mutation == 3)
      Native->removeFnAttr(llvm::Attribute::NoUnwind);
    if (Mutation == 4)
      Call->setMemoryEffects(llvm::MemoryEffects::none());
    if (Mutation == 5)
      Call->addFnAttr(llvm::Attribute::WillReturn);
    if (Mutation == 6)
      Call->addFnAttr(llvm::Attribute::NoReturn);
    const auto Target = Mutation == 7   ? neverd::Arch::X86
                        : Mutation == 8 ? neverd::Arch::AArch64
                                        : neverd::Arch::X64;
    EXPECT_DEATH((void)neverd::classifyX86DivRemNative(*Call, Target),
                 "invalid native x86 divide intrinsic contract");
  }
}

TEST(LLVMCValues, X86DivideIntrinsicsMatchOriginalResultsAndFaults) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires the original Linux x64 DIV/IDIV oracle";
#else
  llvm::LLVMContext Context;
  llvm::Module Module("native-division-intrinsics", Context);
  Module.setTargetTriple(llvm::Triple("x86_64-linux-gnu"));
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(Context);
  std::string Cases = "static const struct { operation fn; unsigned width, "
                      "sign, live; } cases[] = {\n";
  for (unsigned Bits : {8U, 16U, 32U, 64U})
    for (bool Signed : {false, true}) {
      auto *Word = B.getIntNTy(Bits);
      auto *Pair = llvm::StructType::get(Context, {Word, Word});
      auto *Native = llvm::Function::Create(
          llvm::FunctionType::get(Pair, {Word, Word, Word}, false),
          llvm::GlobalValue::ExternalLinkage,
          neverd::x86DivRemNativeName({Bits, Signed}), Module);
      Native->addFnAttr(llvm::Attribute::NoUnwind);
      Native->addFnAttr(llvm::Attribute::Convergent);
      Native->addFnAttr(llvm::Attribute::NoDuplicate);
      for (bool Live : {false, true}) {
        const std::string Name = "native_div_" + std::to_string(Bits) + "_" +
                                 std::to_string(Signed) + "_" +
                                 std::to_string(Live);
        auto *Function = llvm::Function::Create(
            llvm::FunctionType::get(
                B.getVoidTy(),
                {B.getInt64Ty(), B.getInt64Ty(), B.getInt64Ty(), B.getPtrTy()},
                false),
            llvm::GlobalValue::ExternalLinkage, Name, Module);
        Function->getArg(0)->setName("divide_quotient");
        Function->getArg(1)->setName("divide_remainder");
        Function->getArg(2)->setName("divide_divisor");
        B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
        auto *Result = B.CreateCall(
            Native, {B.CreateZExtOrTrunc(Function->getArg(0), Word),
                     B.CreateZExtOrTrunc(Function->getArg(1), Word),
                     B.CreateZExtOrTrunc(Function->getArg(2), Word)});
        if (Live)
          for (unsigned Index : {0U, 1U})
            B.CreateStore(
                B.CreateZExtOrTrunc(B.CreateExtractValue(Result, Index),
                                    B.getInt64Ty()),
                B.CreateConstGEP1_64(B.getInt64Ty(), Function->getArg(3),
                                     Index));
        B.CreateRetVoid();
        Cases += "{" + Name + "," + std::to_string(Bits / 8) + "," +
                 std::to_string(Signed) + "," + std::to_string(Live) + "},\n";
      }
    }
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  neverd::CEmitterOptions Options;
  Options.TheArch = neverd::Arch::X64;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  EXPECT_NE(Source.find("__builtin_ia32_udivrem8"), std::string::npos);
  EXPECT_NE(Source.find("__builtin_ia32_sdivrem64"), std::string::npos);
  llvm::SmallString<256> Oracle(__FILE__);
  llvm::sys::path::remove_dots(Oracle, /*remove_dot_dot=*/true);
  for (unsigned I = 0; I != 3; ++I)
    llvm::sys::path::remove_filename(Oracle);
  llvm::sys::path::append(
      Oracle, "devirtualization/x86_64/fixtures/x86_division_oracle.h");
  Source += "\n#include \"" + Oracle.str().str() + "\"\n";
  Source += "typedef void (*operation)(uint64_t,uint64_t,uint64_t,void*);\n" +
            Cases + "};\n";
  Source += R"C(
static int invoke(operation fn,uint64_t low,uint64_t high,uint64_t divisor,void *out) {
  if(sigsetjmp(division_resume,1)) {division_active=0;return 1;}
  division_active=1; fn(low,high,divisor,out); division_active=0;return 0;
}
int main(void) {
  division_install();
  for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
    unsigned width=cases[i].width,bits=8*width;
    uint64_t mask=UINT64_MAX>>(64-bits), sign=1ULL<<(bits-1);
    uint64_t values[]={0,1,13,mask,sign,sign-1};
    for(unsigned a=0;a<6;++a)for(unsigned d=0;d<6;++d)for(unsigned c=0;c<6;++c) {
      uint64_t low=values[a],high=values[d],divisor=values[c],q=0,r=0;
      int fault=division_native(width,cases[i].sign,
                 width==1?low|(high<<8):low,high,divisor,&q,&r);
      uint64_t result[]={71,73};
      int actual=invoke(cases[i].fn,low,high,divisor,result);
      if(actual!=fault)return 1;
      if(fault||!cases[i].live) {if(result[0]!=71||result[1]!=73)return 2;}
      else {if(result[0]!=(q&mask)||result[1]!=((width==1?q>>8:r)&mask))return 3;}
    }
  }
  return 0;
}
)C";
  for (const char *Level : {"-O0", "-O2"}) {
    compileAndRun(Source, Level, {}, true);
    if (::testing::Test::HasFailure())
      return;
    if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
      compileAndRun(Source, Level, {}, false, false, NeverC);
  }
#endif
}
