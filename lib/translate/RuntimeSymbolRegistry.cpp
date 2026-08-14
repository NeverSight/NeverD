//===- RuntimeSymbolRegistry.cpp - Closed runtime symbol table -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/translate/RuntimeSymbolRegistry.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace neverd::translate {

char RuntimeSymbolRegistryError::ID;

RuntimeSymbolRegistryError::RuntimeSymbolRegistryError(
    RuntimeSymbolRegistryErrorCode Reason, std::string SymbolName)
    : Reason(Reason), SymbolName(std::move(SymbolName)) {}

void RuntimeSymbolRegistryError::log(llvm::raw_ostream &OS) const {
  switch (Reason) {
  case RuntimeSymbolRegistryErrorCode::UnknownSymbol:
    OS << "runtime symbol is not registered";
    break;
  case RuntimeSymbolRegistryErrorCode::InvalidName:
    OS << "runtime symbol has a non-canonical name";
    break;
  case RuntimeSymbolRegistryErrorCode::DuplicateName:
    OS << "runtime symbol is registered more than once";
    break;
  case RuntimeSymbolRegistryErrorCode::BindingNotInABI:
    OS << "runtime symbol is not part of ABI v1";
    break;
  case RuntimeSymbolRegistryErrorCode::MissingBinding:
    OS << "runtime ABI v1 symbol has no binding";
    break;
  case RuntimeSymbolRegistryErrorCode::HelperClassMismatch:
    OS << "runtime symbol helper class does not match its ABI signature";
    break;
  case RuntimeSymbolRegistryErrorCode::InvalidFunctionPointers:
    OS << "runtime symbol must have exactly one class-matching function "
          "pointer";
    break;
  case RuntimeSymbolRegistryErrorCode::NullAddress:
    OS << "runtime symbol has a null native address";
    break;
  case RuntimeSymbolRegistryErrorCode::InvalidABISignature:
    OS << "runtime ABI v1 contains an invalid helper signature";
    break;
  }
  if (!SymbolName.empty())
    OS << ": " << SymbolName;
}

std::error_code RuntimeSymbolRegistryError::convertToErrorCode() const {
  if (Reason == RuntimeSymbolRegistryErrorCode::UnknownSymbol)
    return std::make_error_code(std::errc::no_such_file_or_directory);
  return std::make_error_code(std::errc::invalid_argument);
}

namespace {

llvm::Error failure(RuntimeSymbolRegistryErrorCode Reason,
                    llvm::StringRef Name = {}) {
  return llvm::make_error<RuntimeSymbolRegistryError>(Reason, Name.str());
}

bool isCanonicalRuntimeName(llvm::StringRef Name) {
  constexpr llvm::StringLiteral Prefix("nvd_rt_v1_");
  if (!Name.starts_with(Prefix) || Name.size() == Prefix.size())
    return false;
  return llvm::all_of(Name, [](char C) {
    const unsigned char Byte = static_cast<unsigned char>(C);
    return (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9') ||
           Byte == '_';
  });
}

template <typename T, size_t N>
bool hasExactShape(llvm::ArrayRef<T> Values, const T (&Expected)[N]) {
  return Values.size() == N &&
         std::equal(Values.begin(), Values.end(), std::begin(Expected));
}

llvm::Expected<RuntimeABIHelperClassV1>
classForSignature(const RuntimeABIHelperSignatureV1 &Signature) {
  static constexpr RuntimeABIValueKind LoadParameters[] = {
      RuntimeABIValueKind::RuntimePointer, RuntimeABIValueKind::I64,
      RuntimeABIValueKind::I32};
  static constexpr TranslationRuntimeParameterKind LoadVerifierParameters[] = {
      TranslationRuntimeParameterKind::RuntimePointer,
      TranslationRuntimeParameterKind::ScalarInteger,
      TranslationRuntimeParameterKind::ScalarInteger};
  static constexpr RuntimeABIValueKind StoreParameters[] = {
      RuntimeABIValueKind::RuntimePointer, RuntimeABIValueKind::I64,
      RuntimeABIValueKind::I64, RuntimeABIValueKind::I32};
  static constexpr TranslationRuntimeParameterKind StoreVerifierParameters[] = {
      TranslationRuntimeParameterKind::RuntimePointer,
      TranslationRuntimeParameterKind::ScalarInteger,
      TranslationRuntimeParameterKind::ScalarInteger,
      TranslationRuntimeParameterKind::ScalarInteger};

  if (Signature.Result != RuntimeABIValueKind::I32 ||
      Signature.Parameters.size() != Signature.VerifierParameters.size())
    return failure(RuntimeSymbolRegistryErrorCode::InvalidABISignature,
                   Signature.Name);
  if (hasExactShape(Signature.Parameters, LoadParameters) &&
      hasExactShape(Signature.VerifierParameters, LoadVerifierParameters))
    return RuntimeABIHelperClassV1::Load;
  if (hasExactShape(Signature.Parameters, StoreParameters) &&
      hasExactShape(Signature.VerifierParameters, StoreVerifierParameters))
    return RuntimeABIHelperClassV1::Store;
  return failure(RuntimeSymbolRegistryErrorCode::InvalidABISignature,
                 Signature.Name);
}

const RuntimeABIHelperSignatureV1 *
findSignature(llvm::ArrayRef<RuntimeABIHelperSignatureV1> Signatures,
              llvm::StringRef Name) {
  for (const RuntimeABIHelperSignatureV1 &Signature : Signatures)
    if (Signature.Name == Name)
      return &Signature;
  return nullptr;
}

llvm::Expected<llvm::orc::ExecutorAddr>
addressForBinding(const RuntimeABIHelperBindingV1 &Binding) {
  const bool HasLoad = Binding.Load != nullptr;
  const bool HasStore = Binding.Store != nullptr;
  if (!HasLoad && !HasStore)
    return failure(RuntimeSymbolRegistryErrorCode::NullAddress, Binding.Name);
  if (HasLoad == HasStore ||
      (Binding.Class == RuntimeABIHelperClassV1::Load && !HasLoad) ||
      (Binding.Class == RuntimeABIHelperClassV1::Store && !HasStore))
    return failure(RuntimeSymbolRegistryErrorCode::InvalidFunctionPointers,
                   Binding.Name);

  llvm::orc::ExecutorAddr Address;
  switch (Binding.Class) {
  case RuntimeABIHelperClassV1::Load:
    Address = llvm::orc::ExecutorAddr::fromPtr(Binding.Load);
    break;
  case RuntimeABIHelperClassV1::Store:
    Address = llvm::orc::ExecutorAddr::fromPtr(Binding.Store);
    break;
  }
  if (Address.isNull())
    return failure(RuntimeSymbolRegistryErrorCode::NullAddress, Binding.Name);
  return Address;
}

void appendU16(llvm::SmallVectorImpl<uint8_t> &Bytes, uint16_t Value) {
  Bytes.push_back(static_cast<uint8_t>(Value));
  Bytes.push_back(static_cast<uint8_t>(Value >> 8));
}

void appendU32(llvm::SmallVectorImpl<uint8_t> &Bytes, uint32_t Value) {
  for (unsigned Shift = 0; Shift != 32; Shift += 8)
    Bytes.push_back(static_cast<uint8_t>(Value >> Shift));
}

void appendString(llvm::SmallVectorImpl<uint8_t> &Bytes,
                  llvm::StringRef Value) {
  appendU32(Bytes, static_cast<uint32_t>(Value.size()));
  Bytes.append(Value.bytes_begin(), Value.bytes_end());
}

std::string
registryIdentity(llvm::ArrayRef<RuntimeSymbolEntryV1> Entries,
                 llvm::ArrayRef<RuntimeABIHelperSignatureV1> Signatures) {
  llvm::SmallVector<uint8_t, 256> Shape;
  appendString(Shape, "NeverD.RuntimeSymbolRegistry");
  appendU16(Shape, kRuntimeSymbolRegistryVersionV1);
  appendU32(Shape, kRuntimeABIMagicV1);
  appendU16(Shape, kRuntimeABIVersionV1);
  appendU16(Shape, kRuntimeControlBlockSizeV1);
  appendU32(Shape, static_cast<uint32_t>(Entries.size()));

  for (const RuntimeSymbolEntryV1 &Entry : Entries) {
    const RuntimeABIHelperSignatureV1 *Signature =
        findSignature(Signatures, Entry.name());
    // Construction validates the complete table before identity generation.
    assert(Signature && "validated registry entry has no ABI signature");
    appendString(Shape, Entry.name());
    Shape.push_back(static_cast<uint8_t>(Entry.helperClass()));
    Shape.push_back(static_cast<uint8_t>(Signature->Result));
    appendU32(Shape, static_cast<uint32_t>(Signature->Parameters.size()));
    for (RuntimeABIValueKind Kind : Signature->Parameters)
      Shape.push_back(static_cast<uint8_t>(Kind));
    appendU32(Shape,
              static_cast<uint32_t>(Signature->VerifierParameters.size()));
    for (TranslationRuntimeParameterKind Kind : Signature->VerifierParameters)
      Shape.push_back(static_cast<uint8_t>(Kind));
  }

  const std::array<uint8_t, 32> Digest = llvm::SHA256::hash(Shape);
  return "nvd-runtime-symbol-registry-v1-sha256:" +
         llvm::toHex(llvm::ArrayRef<uint8_t>(Digest), /*LowerCase=*/true);
}

} // namespace

llvm::Expected<RuntimeSymbolRegistryV1> RuntimeSymbolRegistryV1::create() {
  return create(runtimeABIHelperBindingsV1());
}

llvm::Expected<RuntimeSymbolRegistryV1> RuntimeSymbolRegistryV1::create(
    llvm::ArrayRef<RuntimeABIHelperBindingV1> Bindings) {
  const llvm::ArrayRef<RuntimeABIHelperSignatureV1> Signatures =
      runtimeABIHelperSignaturesV1();

  llvm::StringSet<> SignatureNames;
  for (const RuntimeABIHelperSignatureV1 &Signature : Signatures) {
    if (!isCanonicalRuntimeName(Signature.Name) ||
        !SignatureNames.insert(Signature.Name).second)
      return failure(RuntimeSymbolRegistryErrorCode::InvalidABISignature,
                     Signature.Name);
    llvm::Expected<RuntimeABIHelperClassV1> Class =
        classForSignature(Signature);
    if (!Class)
      return Class.takeError();
  }

  llvm::StringSet<> BoundNames;
  std::vector<RuntimeSymbolEntryV1> Entries;
  Entries.reserve(Bindings.size());
  for (const RuntimeABIHelperBindingV1 &Binding : Bindings) {
    if (!isCanonicalRuntimeName(Binding.Name))
      return failure(RuntimeSymbolRegistryErrorCode::InvalidName, Binding.Name);
    if (!BoundNames.insert(Binding.Name).second)
      return failure(RuntimeSymbolRegistryErrorCode::DuplicateName,
                     Binding.Name);

    const RuntimeABIHelperSignatureV1 *Signature =
        findSignature(Signatures, Binding.Name);
    if (!Signature)
      return failure(RuntimeSymbolRegistryErrorCode::BindingNotInABI,
                     Binding.Name);
    llvm::Expected<RuntimeABIHelperClassV1> ExpectedClass =
        classForSignature(*Signature);
    if (!ExpectedClass)
      return ExpectedClass.takeError();
    if (Binding.Class != *ExpectedClass)
      return failure(RuntimeSymbolRegistryErrorCode::HelperClassMismatch,
                     Binding.Name);

    llvm::Expected<llvm::orc::ExecutorAddr> Address =
        addressForBinding(Binding);
    if (!Address)
      return Address.takeError();
    Entries.push_back(
        RuntimeSymbolEntryV1(Binding.Name.str(), Binding.Class, *Address));
  }

  for (const RuntimeABIHelperSignatureV1 &Signature : Signatures)
    if (!BoundNames.contains(Signature.Name))
      return failure(RuntimeSymbolRegistryErrorCode::MissingBinding,
                     Signature.Name);

  std::sort(
      Entries.begin(), Entries.end(),
      [](const RuntimeSymbolEntryV1 &Left, const RuntimeSymbolEntryV1 &Right) {
        return Left.name() < Right.name();
      });
  std::string Identity = registryIdentity(Entries, Signatures);
  return RuntimeSymbolRegistryV1(std::move(Entries), std::move(Identity));
}

std::vector<llvm::StringRef> RuntimeSymbolRegistryV1::names() const {
  std::vector<llvm::StringRef> Names;
  Names.reserve(Entries.size());
  for (const RuntimeSymbolEntryV1 &Entry : Entries)
    Names.push_back(Entry.name());
  return Names;
}

std::vector<llvm::StringRef>
RuntimeSymbolRegistryV1::artifactVerifierAllowlist() const {
  return names();
}

llvm::Expected<llvm::orc::ExecutorAddr>
RuntimeSymbolRegistryV1::lookup(llvm::StringRef Name) const {
  const auto It = std::lower_bound(
      Entries.begin(), Entries.end(), Name,
      [](const RuntimeSymbolEntryV1 &Entry, llvm::StringRef Candidate) {
        return Entry.name() < Candidate;
      });
  if (It == Entries.end() || It->name() != Name)
    return failure(RuntimeSymbolRegistryErrorCode::UnknownSymbol, Name);
  return It->address();
}

} // namespace neverd::translate
