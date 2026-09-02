//===- NeverDTranslateOutput.cpp - Translation output support ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDTranslateOutput.h"

#include "llvm/Support/Errc.h"

namespace neverd::cli::detail {

llvm::Error validateTranslationResultBindingV1(
    const neverd_translate_object_request_v1 &Request,
    const neverd_translate_object_result_v1 &Result) {
  if (Result.guest_entry_pc != Request.entry_pc)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "translation returned an object for an unexpected guest entry");
  if (Result.executable_generation != Request.executable_generation)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "translation returned an object for an unexpected executable "
        "generation");
  return llvm::Error::success();
}

} // namespace neverd::cli::detail
