//===- TranslationCacheIdentity.h - Stable translation hashing -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_TRANSLATIONCACHEIDENTITY_H
#define NEVERD_LIB_TRANSLATE_TRANSLATIONCACHEIDENTITY_H

#include "neverd/translate/TranslationObjectCompiler.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SHA256.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace neverd::translate::detail {

uint64_t stableSize(std::size_t Value);

/// Locale- and host-layout-independent SHA-256 input writer.  Every variable
/// length field carries a little-endian u64 length and every scalar has an
/// explicitly selected width.
class StableHashWriter final {
public:
  void addByte(uint8_t Value);
  void addBool(bool Value);
  void addU16(uint16_t Value);
  void addU32(uint32_t Value);
  void addI32(int32_t Value);
  void addU64(uint64_t Value);
  void addDouble(double Value);
  void addString(llvm::StringRef Value);
  void addBytes(llvm::ArrayRef<uint8_t> Value);

  std::string finish(llvm::StringRef Prefix);

private:
  llvm::SHA256 Hash;
};

void hashTranslationOptions(StableHashWriter &Hash,
                            const TranslationOptions &Options,
                            const ResolvedHostTarget &Target);

void hashSemanticPolicy(StableHashWriter &Hash,
                        const TranslationSemanticPolicyV1 &Policy);

void hashMemorySlots(StableHashWriter &Hash,
                     llvm::ArrayRef<TranslationIRMemorySlot> Slots);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_TRANSLATIONCACHEIDENTITY_H
