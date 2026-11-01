//===- LLVMCFrameAccessTests.cpp - Typed frame reads -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep typed frame members in the LLVM load's pointer or integer carrier.
///
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/debug/DebugContext.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>
#include <vector>

using namespace neverd;

namespace {
class FrameRecordDebug : public NullDebugContext {
public:
  std::optional<VariableSym> resolveVariable(va_t Function,
                                             int64_t Offset) const override {
    if (Function != 0x140001000 || Offset != -16)
      return std::nullopt;
    auto Record = NdType::makeNamedRecord("Readback", 16);
    Record->FieldDisplayNames = {"pointer", "bits"};
    Record->FieldDisplayOffsets = {0, 8};
    Record->FieldDisplayTypes = {NdType::makePtr(NdType::makeVoid()),
                                 NdType::makeInt(8, false)};
    VariableSym Variable;
    Variable.Name = "record";
    Variable.Type = Record;
    Variable.StackOffset = Offset;
    return Variable;
  }
  bool hasInfo() const override { return true; }
};
} // namespace

TEST(LLVMCFrameAccess, DeclaredMembersKeepLoadCarriersAcrossMutation) {
  // One read must be captured before refill; the later read can be composed
  // at its use. Both use the LLVM carrier even when the C member differs.
  for (const BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    for (bool PointerLoad : {false, true}) {
      SCOPED_TRACE(PointerLoad);
      FrameRecordDebug Debug;
      llvm::LLVMContext Context;
      llvm::Module Module("frame-member-carriers", Context);
      auto *I8 = llvm::Type::getInt8Ty(Context);
      auto *I64 = llvm::Type::getInt64Ty(Context);
      auto *Pointer = llvm::PointerType::getUnqual(Context);
      auto *FillType = llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                               {Pointer}, false);
      auto *Fill = llvm::Function::Create(
          FillType, llvm::GlobalValue::ExternalLinkage, "fill", Module);
      auto *Refill = llvm::Function::Create(
          FillType, llvm::GlobalValue::ExternalLinkage, "refill", Module);
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(I64, false),
          llvm::GlobalValue::ExternalLinkage, "read_frame", Module);
      rewrite_source::setOriginalVA(*Function, 0x140001000);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Frame =
          Builder.CreateAlloca(llvm::ArrayType::get(I8, 64), nullptr, "frame");
      auto *End = Builder.CreateInBoundsGEP(
          I8, Frame, llvm::ConstantInt::get(I64, 40), "frame_end");
      auto *Rsp = Builder.CreatePtrToInt(End, I64, "rsp_init");
      auto *Home = Builder.CreateIntToPtr(
          Builder.CreateSub(Rsp, llvm::ConstantInt::get(I64, 16)), Pointer,
          "home");
      auto *Address =
          PointerLoad
              ? Builder.CreateGEP(I8, Home, llvm::ConstantInt::get(I64, 8))
              : Home;
      llvm::Type *Carrier =
          PointerLoad ? static_cast<llvm::Type *>(Pointer) : I64;
      Builder.CreateCall(Fill, {Home});
      auto *Before = Builder.CreateLoad(Carrier, Address, "before");
      Builder.CreateCall(Refill, {Home});
      auto *After = Builder.CreateLoad(Carrier, Address, "after");
      const auto Bits = [&](llvm::Value *Value) {
        return PointerLoad ? Builder.CreatePtrToInt(Value, I64) : Value;
      };
      Builder.CreateRet(Builder.CreateSub(Bits(After), Bits(Before)));

      std::string Source;
      llvm::raw_string_ostream Stream(Source);
      CEmitterOptions Options;
      Options.EmitIncludes = false;
      Options.TheArch = Arch::X64;
      Options.Format = Format;
      Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
        CRecordDefinitions Definitions;
        Definitions.Text =
            "struct Readback { void *pointer; uint64_t bits; };\n";
        Definitions.Records = {{"Readback", false}};
        Definitions.Tags = {{"Readback", false}};
        return Definitions;
      };
      ASSERT_TRUE(LLVMCEmitter().emit(Module, Stream, Options, &Debug, nullptr,
                                      Function));
      Stream.flush();
      const auto Read = Source.find("before0 = ");
      ASSERT_NE(Read, std::string::npos) << Source;
      EXPECT_LT(Read, Source.find("refill(&record)")) << Source;
      EXPECT_NE(Source.find(PointerLoad ? "record.bits" : "record.pointer"),
                std::string::npos)
          << Source;
      const std::string Program = R"c(
#include <stdint.h>
typedef struct Readback { void *pointer; uint64_t bits; } Readback;
void fill(void *);
void refill(void *);
)c" + Source + R"c(
static unsigned char values[16];
static unsigned fills, refills;
void fill(void *out) {
  struct Readback *record = out;
  ++fills;
  record->pointer = &values[1];
  record->bits = (uintptr_t)&values[1];
}
void refill(void *out) {
  struct Readback *record = out;
  ++refills;
  record->pointer = &values[6];
  record->bits = (uintptr_t)&values[6];
}
int main(void) {
  return read_frame() != 5 || fills != 1 || refills != 1;
}
)c";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Program, {Level, "-Werror=int-conversion",
                      "-Werror=incompatible-pointer-types"});
    }
  }
}
