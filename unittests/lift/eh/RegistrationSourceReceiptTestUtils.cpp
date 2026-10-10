//===- RegistrationSourceReceiptTestUtils.cpp - PE32 receipts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind reconstruction evidence to original and generated image identities.
//===----------------------------------------------------------------------===//
#include "RegistrationSourceReceiptTestUtils.h"

#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>

namespace neverd::registration_test {
void writeSourceReceipt(const char *Input, const char *Output,
                        const BinaryImage &Image,
                        const ExceptionFunction &Source,
                        const RegistrationStateAnalysis &States,
                        const ExceptionFunction &Generated,
                        llvm::json::Object Context) {
  const auto *Receipt = std::getenv("NEVERD_REGISTRATION_REALIGNED_RECEIPT");
  if (!Receipt)
    return;
  auto Digest = [](const char *File) {
    auto Buffer = llvm::MemoryBuffer::getFile(File);
    EXPECT_TRUE(bool(Buffer));
    if (!Buffer)
      return std::string();
    return llvm::toHex(
        llvm::SHA256::hash(llvm::arrayRefFromStringRef((*Buffer)->getBuffer())),
        true);
  };
  Context["incoming_reads"] = llvm::count_if(
      States.IncomingFrameAccesses, [](const auto &A) { return !A.Write; });
  Context["incoming_writes"] = llvm::count_if(
      States.IncomingFrameAccesses, [](const auto &A) { return A.Write; });
  Context["schema"] = 1;
  Context["evidence"] = "checked-realigned-source-reconstruction";
  Context["source_frame"] = Source.Registration->RealignedFrame ? "realigned"
                            : Source.Registration->hasCxxCallbackStack()
                                ? "fixed-displaced"
                                : "direct";
  Context["source_image_sha256"] = Digest(Input);
  Context["image_sha256"] = Digest(Output);
  Context["base"] = Image.Base;
  Context["source_begin"] = Source.CodeRange.Begin - Image.Base;
  Context["source_end"] = Source.CodeRange.End - Image.Base;
  Context["generated_begin"] = Generated.CodeRange.Begin - Image.Base;
  Context["generated_end"] = Generated.CodeRange.End - Image.Base;
  std::error_code Error;
  llvm::raw_fd_ostream Out(Receipt, Error);
  ASSERT_FALSE(Error) << Error.message();
  Out << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(Context)));
}
} // namespace neverd::registration_test
