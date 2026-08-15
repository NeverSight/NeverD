//===- TranslationBlockLowerer.h - Canonical block IR lowering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the fail-closed x86-64 block-to-host-IR boundary.  The lowered
/// module uses the fixed translation runtime ABI and retains guest addresses
/// as integers; publication and execution are separate stages.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONBLOCKLOWERER_H
#define NEVERD_TRANSLATE_TRANSLATIONBLOCKLOWERER_H

#include "neverd/translate/ResolvedHostTarget.h"
#include "neverd/translate/TranslationBlock.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace llvm {
class DataLayout;
class LLVMContext;
class Module;
class raw_ostream;
} // namespace llvm

namespace neverd::translate {

inline constexpr uint32_t kX86TranslationBlockLoweringSchemaV1 = 9;

/// Stable failures from the version-1 x86-64 block lowerer.  Append values
/// without renumbering existing entries.
enum class TranslationBlockLoweringErrorCode : uint8_t {
  InvalidDescriptor = 0,
  UnsupportedHostTarget = 1,
  InvalidHostDataLayout = 2,
  UnsupportedBlockShape = 3,
  UnsupportedOperation = 4,
  InvalidOperand = 5,
  UnsupportedRegister = 6,
  UndefinedTemporary = 7,
  InvalidControlFlow = 8,
  IRVerificationFailed = 9,
};

class TranslationBlockLoweringError final
    : public llvm::ErrorInfo<TranslationBlockLoweringError> {
public:
  static char ID;

  TranslationBlockLoweringError(TranslationBlockLoweringErrorCode Code,
                                uint64_t GuestPC,
                                std::optional<uint64_t> OpIndex = std::nullopt,
                                std::optional<NdOp> Opcode = std::nullopt,
                                std::string Detail = {});

  TranslationBlockLoweringErrorCode code() const { return Code; }
  uint64_t guestPC() const { return GuestPC; }
  std::optional<uint64_t> opIndex() const { return OpIndex; }
  std::optional<NdOp> opcode() const { return Opcode; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  TranslationBlockLoweringErrorCode Code;
  uint64_t GuestPC;
  std::optional<uint64_t> OpIndex;
  std::optional<NdOp> Opcode;
  std::string Detail;
};

/// Owned canonical host IR for one byte-and-semantics-verified translation
/// block.
class LoweredTranslationBlockV1 final {
public:
  LoweredTranslationBlockV1(LoweredTranslationBlockV1 &&) noexcept;
  LoweredTranslationBlockV1 &operator=(LoweredTranslationBlockV1 &&) noexcept;
  LoweredTranslationBlockV1(const LoweredTranslationBlockV1 &) = delete;
  LoweredTranslationBlockV1 &
  operator=(const LoweredTranslationBlockV1 &) = delete;
  ~LoweredTranslationBlockV1();

  llvm::Module &module() { return *Module; }
  const llvm::Module &module() const { return *Module; }
  llvm::StringRef blockSymbol() const { return BlockSymbol; }

  std::unique_ptr<llvm::Module> takeModule() &&;

private:
  friend llvm::Expected<LoweredTranslationBlockV1>
  lowerX86TranslationBlockV1(const TranslationBlockDescriptorV1 &,
                             const ResolvedHostTarget &,
                             const llvm::DataLayout &, llvm::LLVMContext &);

  LoweredTranslationBlockV1(std::unique_ptr<llvm::Module> Module,
                            std::string BlockSymbol);

  std::unique_ptr<llvm::Module> Module;
  std::string BlockSymbol;
};

/// Lower the published version-1 x86-64 scalar-register subset into one
/// canonical AArch64 module.  Lowering schema 9 covers full-width GPR moves,
/// add/subtract and AND/OR/XOR with their architecturally defined scalar flags;
/// full-width register-only CMP 39/3B and register/immediate CMP 81/7, 83/7,
/// and 3D; full-width register-only TEST 85 and register/immediate TEST F7/0
/// and A9; return/return-immediate; canonical rel8/rel32 direct jumps; and
/// canonical
/// legacy-prefix-free traditional Jcc in rel8/rel32 form: JO/JNO, JB/JAE,
/// JE/JNE, JBE/JA, JS/JNS, JP/JNP, JL/JGE, and JLE/JG. TEST preserves AF in
/// the NeverD state model. Reserved F7 /1, guest-memory and partial-register
/// forms, legacy prefixes, semantically redundant REX bits,
/// JRCXZ/JECXZ/JCXZ, and LOOP/LOOPE/LOOPNE remain unpublished. The boundary
/// independently rebuilds canonical LowIR from Block.Bytes
/// and rejects any caller-supplied semantic mismatch.  Published direct and
/// conditional transfers return to the dispatcher after committing
/// a manifest-authorized guest successor; they never link one translated block
/// directly to another.
/// HostTarget and HostDataLayout must be the exact paired code-generation
/// inputs from one
/// TranslationTargetMachineV1.  A standalone ResolvedHostTarget or any layout
/// other than that machine's canonical layout fails closed.
llvm::Expected<LoweredTranslationBlockV1>
lowerX86TranslationBlockV1(const TranslationBlockDescriptorV1 &Block,
                           const ResolvedHostTarget &HostTarget,
                           const llvm::DataLayout &HostDataLayout,
                           llvm::LLVMContext &Context);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONBLOCKLOWERER_H
