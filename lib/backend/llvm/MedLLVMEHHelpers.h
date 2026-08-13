//===- MedLLVMEHHelpers.h - MedLLVM EH helpers ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal metadata helpers shared by the MedLLVM EH translation units.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_LLVM_MEDLLVMEHHELPERS_H
#define NEVERD_LIB_BACKEND_LLVM_MEDLLVMEHHELPERS_H

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Metadata.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::med_llvm_eh {

inline llvm::Metadata *mdUInt(llvm::LLVMContext &Ctx, uint64_t Value,
                              unsigned Bits = 64) {
  return llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::get(llvm::IntegerType::get(Ctx, Bits), Value));
}

inline llvm::Metadata *mdSInt(llvm::LLVMContext &Ctx, int64_t Value,
                              unsigned Bits = 64) {
  return llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::getSigned(llvm::IntegerType::get(Ctx, Bits), Value));
}

inline std::string hexBytes(const std::vector<uint8_t> &Bytes) {
  static constexpr char Digits[] = "0123456789abcdef";
  std::string Result;
  Result.reserve(Bytes.size() * 2);
  for (uint8_t Byte : Bytes) {
    Result.push_back(Digits[Byte >> 4]);
    Result.push_back(Digits[Byte & 0x0f]);
  }
  return Result;
}

} // namespace neverd::med_llvm_eh

#endif // NEVERD_LIB_BACKEND_LLVM_MEDLLVMEHHELPERS_H
