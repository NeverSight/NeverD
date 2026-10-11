//===- InterpreterLLVMRefinementTest.h - Native/LLVM fixtures ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_INTERPRETERLLVMREFINEMENTTEST_H
#define NEVERD_UNITTESTS_INTERPRETERLLVMREFINEMENTTEST_H

#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterLLVMRefinement.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::analysis::llvm_refinement_test {
constexpr va_t Entry = 0x1000;
using Stage = InterpreterLLVMRefinementStage;
using Status = LowIRRefinementStatus;

inline std::string module(llvm::StringRef Body,
                          llvm::StringRef Name = "model") {
  return "target datalayout = \"e-p:64:64-i64:64-n8:16:32:64\"\n"
         "target triple = \"x86_64-unknown-linux-gnu\"\n"
         "define i64 @" +
         Name.str() + "(ptr %state) {\n" + Body.str() + "\n}\n";
}

struct Program {
  BinaryImage Image;
  SpecializationOptions Options;
  LowIRIndependenceFrame Frame{{x86reg::RSP, 8}, -64, 8};

  Program(std::initializer_list<uint8_t> Bytes) {
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
    Segment Code;
    Code.VA = Entry;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data = Bytes;
    Code.Size = Code.FileSz = Code.Data.size();
    Image.Segments.push_back(std::move(Code));
    Options.ExplicitMachineState = true;
    Options.NormalNonfaultingExecution = true;
    Options.X64CetDisabled = true;
    Options.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
    Options.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
  }

  SpecializationResult recover() const {
    return specializeBinaryInterpreter(Image, Entry, Options);
  }
  InterpreterLLVMRefinementResult
  check(const LowFunc &Residual, llvm::StringRef IR,
        const InterpreterLLVMRefinementLimits &Limits = {},
        const InterpreterLLVMRefinementPlans &Plans = {},
        llvm::StringRef Name = "model",
        const InterpreterLLVMRefinementPreservation &Preservation = {},
        const InterpreterLLVMNativeCollection &Collection = {}) const {
    return checkBinaryLLVMRefinement(
        Image, Entry, Options, Residual, IR, Name, Frame, Plans,
        LowIRRefinementWitness::LiftedBits, Limits, Preservation, Collection);
  }
};

inline void rejected(const InterpreterLLVMRefinementResult &R, Stage S) {
  EXPECT_EQ(R.Stage, S) << R.Diagnostic;
  EXPECT_FALSE(R.proved());
  EXPECT_FALSE(R.Certificate);
}
} // namespace neverd::analysis::llvm_refinement_test
#endif
