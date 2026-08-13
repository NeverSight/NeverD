//===- EVMInterpreterDetail.h - Private EVM interpreter helpers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the environment validation and the word, memory, and map
/// primitives the interpreter's dispatch loop is written in terms of.
///
/// This is an implementation detail of lib/evm/runtime. Nothing outside that
/// directory may include it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_EVM_RUNTIME_EVMINTERPRETERDETAIL_H
#define NEVERD_LIB_EVM_RUNTIME_EVMINTERPRETERDETAIL_H

#include "neverd/evm/runtime/EVMInterpreter.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::evm::detail {

llvm::APInt zeroWord();
llvm::APInt boolWord(bool Value);

/// Reject an environment whose words are not machine words and whose address
/// fields do not fit an address, before a single instruction runs.
llvm::Error validateEnvironment(const ExecutionEnvironment &Environment);

std::optional<size_t> toSize(const llvm::APInt &Word, size_t Limit);
bool checkedRange(size_t Offset, size_t Size, size_t Limit, size_t &End);

llvm::APInt bytesToWord(const std::vector<uint8_t> &Bytes, size_t Offset);
void wordToBytes(const llvm::APInt &Word, uint8_t *Output);

llvm::APInt mapLookup(const WordMap &Map, const llvm::APInt &Key);
llvm::APInt canonicalAddress(const llvm::APInt &Word);

std::vector<uint8_t>::iterator byteIterator(std::vector<uint8_t> &Bytes,
                                            size_t Offset);
std::vector<uint8_t>::const_iterator
byteIterator(const std::vector<uint8_t> &Bytes, size_t Offset);

const std::vector<uint8_t> &codeLookup(const BytecodeMap &Map,
                                       const llvm::APInt &Address);

} // namespace neverd::evm::detail

#endif // NEVERD_LIB_EVM_RUNTIME_EVMINTERPRETERDETAIL_H
