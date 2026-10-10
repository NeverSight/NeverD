//===- MedLLVMRegistrationCxxCall.h - PE32 calls --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind source call evidence to the current LLVM call before native lowering.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONCXXCALL_H
#define NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONCXXCALL_H

#include "llvm/IR/CallingConv.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace llvm {
class CallInst;
class FunctionType;
} // namespace llvm
namespace neverd {
struct BinaryImage;
struct RegistrationCalleeFrameContract;
struct RegistrationCallFrameEffect;
namespace med_llvm_eh {
struct RegistrationCxxCallABI {
  llvm::FunctionType *Type = nullptr;
  llvm::CallingConv::ID Convention = llvm::CallingConv::C;
  bool BorrowsECX = false;
  bool RuntimeThrow = false;
  uint64_t ThrowInfoVA = 0;
};
std::optional<RegistrationCxxCallABI> getCheckedRegistrationCxxCallABI(
    const BinaryImage &Image, const RegistrationCalleeFrameContract &Contract,
    const RegistrationCallFrameEffect &Effect, const llvm::CallInst &Call,
    size_t &Work);
} // namespace med_llvm_eh
} // namespace neverd
#endif
