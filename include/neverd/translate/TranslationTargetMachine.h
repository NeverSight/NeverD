//===- TranslationTargetMachine.h - Exact host code generation -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Owns the single resolved LLVM target-machine boundary shared by lowering
/// and object emission.  Consumers must use dataLayout() rather than spelling
/// a target layout independently.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONTARGETMACHINE_H
#define NEVERD_TRANSLATE_TRANSLATIONTARGETMACHINE_H

#include "neverd/translate/ResolvedHostTarget.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <system_error>

namespace llvm {
class TargetMachine;
class raw_ostream;
} // namespace llvm

namespace neverd::translate {

namespace detail {
class TranslationObjectCompilerAccess;
} // namespace detail

/// Stable target-machine construction failures.  Append without renumbering.
enum class TranslationTargetMachineErrorCode : uint8_t {
  HostTargetResolutionFailed = 0,
  UnsupportedHostArchitecture = 1,
  TargetLookupFailed = 2,
  TargetCPUOrFeatureRejected = 3,
  TargetMachineCreationFailed = 4,
};

class TranslationTargetMachineError final
    : public llvm::ErrorInfo<TranslationTargetMachineError> {
public:
  static char ID;

  TranslationTargetMachineError(TranslationTargetMachineErrorCode Code,
                                std::string Detail = {});

  TranslationTargetMachineErrorCode code() const { return Code; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  TranslationTargetMachineErrorCode Code;
  std::string Detail;
};

/// Immutable target identity and exact DataLayout paired with one LLVM target
/// machine.  The machine is intentionally not recreated between lowering and
/// emission, so target defaults cannot drift across the two stages.
class TranslationTargetMachineV1 final {
public:
  TranslationTargetMachineV1(TranslationTargetMachineV1 &&) noexcept;
  TranslationTargetMachineV1 &operator=(TranslationTargetMachineV1 &&) noexcept;
  TranslationTargetMachineV1(const TranslationTargetMachineV1 &) = delete;
  TranslationTargetMachineV1 &
  operator=(const TranslationTargetMachineV1 &) = delete;
  ~TranslationTargetMachineV1();

  const ResolvedHostTarget &hostTarget() const { return HostTarget; }
  const llvm::DataLayout &dataLayout() const { return DataLayout; }

  /// True when Options would construct this exact code-generation machine.
  bool matchesCodeGenerationOptions(const TranslationOptions &Options) const;

private:
  friend class detail::TranslationObjectCompilerAccess;
  friend llvm::Expected<TranslationTargetMachineV1>
  createTranslationTargetMachineV1(const TranslationOptions &);

  llvm::TargetMachine &targetMachine() { return *Machine; }

  TranslationTargetMachineV1(ResolvedHostTarget HostTarget,
                             std::unique_ptr<llvm::TargetMachine> Machine,
                             const TranslationOptions &Options);

  ResolvedHostTarget HostTarget;
  std::unique_ptr<llvm::TargetMachine> Machine;
  llvm::DataLayout DataLayout;
  GuestArchitecture Guest;
  TranslationMode Mode;
  TranslationOptimizationPolicy Optimization;
  LLVMOptimizationLevel OptimizationLevel;
};

llvm::Expected<TranslationTargetMachineV1>
createTranslationTargetMachineV1(const TranslationOptions &Options);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONTARGETMACHINE_H
