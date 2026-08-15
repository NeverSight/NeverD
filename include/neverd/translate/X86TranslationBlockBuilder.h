//===- X86TranslationBlockBuilder.h - Build exact x86 blocks --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86TRANSLATIONBLOCKBUILDER_H
#define NEVERD_TRANSLATE_X86TRANSLATIONBLOCKBUILDER_H

#include "neverd/translate/TranslationBlock.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace neverd {
class Decoder;
}

namespace neverd::translate {

/// Stable, fail-closed construction failures.  Append without renumbering.
enum class X86TranslationBlockBuilderErrorCode : uint8_t {
  DecoderInitializationFailed = 0,
  InstructionFetchFailed = 1,
  TruncatedInstruction = 2,
  UndecodableInstruction = 3,
  UnliftedInstruction = 4,
  GuestAddressOverflow = 5,
  InconsistentDecode = 6,
  InconsistentControl = 7,
  ExecutableBytesChanged = 8,
  InvalidDescriptor = 9,
  InstructionBudgetExceeded = 10,
};

class X86TranslationBlockBuilderError final
    : public llvm::ErrorInfo<X86TranslationBlockBuilderError> {
public:
  static char ID;

  X86TranslationBlockBuilderError(
      X86TranslationBlockBuilderErrorCode Code, uint64_t GuestPC,
      std::optional<GuestMemoryFault> Fault = std::nullopt,
      std::string Detail = {});

  X86TranslationBlockBuilderErrorCode code() const { return Code; }
  uint64_t guestPC() const { return GuestPC; }
  std::optional<RuntimeMemoryFaultKindV1> fault() const {
    return Fault ? std::optional<RuntimeMemoryFaultKindV1>(Fault->Kind)
                 : std::nullopt;
  }
  const std::optional<GuestMemoryFault> &faultDetails() const { return Fault; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  X86TranslationBlockBuilderErrorCode Code;
  uint64_t GuestPC;
  std::optional<GuestMemoryFault> Fault;
  std::string Detail;
};

/// Per-session/thread x86-64 block builder.  Decoder state is never shared
/// across threads.  build() returns no partial descriptor on any failure.
class X86TranslationBlockBuilder final {
public:
  static llvm::Expected<std::unique_ptr<X86TranslationBlockBuilder>> create();

  X86TranslationBlockBuilder(const X86TranslationBlockBuilder &) = delete;
  X86TranslationBlockBuilder &
  operator=(const X86TranslationBlockBuilder &) = delete;
  ~X86TranslationBlockBuilder();

  llvm::Expected<TranslationBlockDescriptorV1>
  build(GuestMemoryRuntime &Memory, uint64_t EntryPC,
        uint64_t InstructionBudget = 0);

private:
  explicit X86TranslationBlockBuilder(std::unique_ptr<Decoder> Decoder);

  std::unique_ptr<Decoder> TheDecoder;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86TRANSLATIONBLOCKBUILDER_H
