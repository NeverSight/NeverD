//===- RegistrationImageProof.h - Current PE32 image writes ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind image aliases to original byte intervals and check current writes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_REGISTRATIONIMAGEPROOF_H
#define NEVERD_BACKEND_LLVM_REGISTRATIONIMAGEPROOF_H

#include "neverd/backend/llvm/RegistrationFrameProofAddress.h"
#include "neverd/loader/ExceptionCommon.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace llvm {
class CallBase;
class Module;
} // namespace llvm

namespace neverd::registration_frame {

/// The original PE32 interval denoted by an external image declaration.
/// Different declaration names can denote overlapping original storage.
/// Mapping, permissions and source occurrence ownership remain separate proofs.
std::optional<ExceptionAddressRange>
imageRange(const Cell &Where, uint64_t Bytes, va_t SecurityCookieVA = 0);

/// Recover the PE32 address of one actual call argument only when it is a
/// direct ptrtoint of the module's canonical external original-data global.
/// Preferred integer bits, loaded stack contents and aliases supply no such
/// identity. This proves the current argument expression only; the source
/// invocation, mapped storage and final image still need independent binding.
std::optional<uint32_t> originalImageArgumentAddress(const llvm::CallBase &Call,
                                                     unsigned Index,
                                                     size_t &Work);

class ImageProof {
public:
  /// Authenticate effects not resolved as an explicit image/private store.
  /// True requires preservation of this entire original image interval,
  /// including exceptional effects. Editable LLVM memory attributes are not
  /// authority. This projection must not assume the result of this query.
  using WriteProjection = llvm::function_ref<bool(
      const llvm::Instruction &, const ExceptionAddressRange &)>;

  /// The indexed functions, IR and projections must remain unchanged and
  /// outlive this query. Include every runtime callback whose writes matter.
  /// Address/borrow projections still require their complete alias/use closure.
  ImageProof(AddressProof &Addresses,
             llvm::ArrayRef<const llvm::Function *> Functions,
             va_t SecurityCookieVA, size_t &Work, bool &Exhausted,
             WriteProjection Project = {})
      : Addresses(Addresses), Functions(Functions),
        SecurityCookieVA(SecurityCookieVA), Work(Work), Exhausted(Exhausted),
        Project(Project) {}

  /// Check every current write, rather than equating two image observations
  /// by symbol name. Unknown effects, aliases or exhausted work reject.
  bool preserves(const ExceptionAddressRange &Range);

  /// Check all CFG paths in Before's indexed function up to, but excluding,
  /// this exact instruction. A backedge also checks the previous iteration's
  /// suffix, including Before itself. The function must belong to Functions
  /// and have an entry path to Before. Independently authenticated call
  /// projections own exceptional/runtime callback effects; this prefix query
  /// does not replace the separate whole-image/callback closure.
  bool preservesBefore(const ExceptionAddressRange &Range,
                       const llvm::Instruction &Before);

private:
  bool preserves(const llvm::Instruction &I,
                 const ExceptionAddressRange &Range);
  bool charge();
  AddressProof &Addresses;
  llvm::ArrayRef<const llvm::Function *> Functions;
  va_t SecurityCookieVA;
  size_t &Work;
  bool &Exhausted;
  WriteProjection Project;
  std::map<std::pair<va_t, va_t>, bool> Preserved;
};

} // namespace neverd::registration_frame

#endif // NEVERD_BACKEND_LLVM_REGISTRATIONIMAGEPROOF_H
