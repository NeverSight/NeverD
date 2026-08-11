//===- Keccak.h - Ethereum's Keccak-256 permutation -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the private Keccak-256 primitive.
///
/// Ethereum's hash differs from standardized SHA3-256 only in its domain
/// separator, so no LLVM support library provides it. The hashing opcode and
/// the ABI's selector derivation are the same function applied to different
/// inputs, and both read it from here rather than carrying a copy.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_EVM_KECCAK_H
#define NEVERD_LIB_EVM_KECCAK_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace neverd::evm {

inline constexpr size_t kKeccak256DigestBytes = 32;

using Keccak256Digest = std::array<uint8_t, kKeccak256DigestBytes>;

Keccak256Digest keccak256(llvm::ArrayRef<uint8_t> Data);
Keccak256Digest keccak256(llvm::StringRef Text);

/// The digest read as the big-endian machine word the hashing opcode pushes.
llvm::APInt keccak256Word(llvm::ArrayRef<uint8_t> Data);

/// The leading four bytes of the digest read as the big-endian selector an ABI
/// call places at the start of its calldata.
uint32_t keccak256Selector(llvm::StringRef Text);

/// The whole digest read as the big-endian word a log places in its first
/// topic.
llvm::APInt keccak256Topic(llvm::StringRef Text);

} // namespace neverd::evm

#endif // NEVERD_LIB_EVM_KECCAK_H
