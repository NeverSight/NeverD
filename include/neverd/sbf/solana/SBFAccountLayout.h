//===- SBFAccountLayout.h - Buffer the loader hands a program ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the two shapes of the serialized input a Solana program is started
/// with.
///
/// A program receives one pointer and reads everything through it, so which
/// field a given offset names is the difference between recovering
/// `account.key` and recovering `account.is_writable`. There are two
/// serializations and they order the fields differently, so the answer depends
/// on the loader that owns the program — a fact that is nowhere in the program
/// file. Reading one as the other lands on a real field of the wrong meaning
/// and produces output that looks right.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SOLANA_SBFACCOUNTLAYOUT_H
#define NEVERD_SBF_SOLANA_SBFACCOUNTLAYOUT_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::sbf {

/// Which serialization the loader that owns a program produces.
enum class AccountABI : uint8_t {
#define SBF_ACCOUNT_ABI(ID, NAME, ALIGNED, DUPLICATE_ENTRY_SIZE, SUMMARY) ID,
#include "neverd/sbf/solana/SBFAccountLayout.def"
};

struct AccountABIInfo {
  AccountABI ID;
  llvm::StringLiteral Name;
  bool Aligned;
  /// How many bytes an entry that repeats an earlier account occupies. A walk
  /// that assumes the wrong size is misaligned for every entry after the
  /// first repeat.
  uint64_t DuplicateEntrySize;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<AccountABIInfo> accountABIInfos();
const AccountABIInfo &getAccountABIInfo(AccountABI ABI);
llvm::StringRef accountABIName(AccountABI ABI);
std::optional<AccountABI> parseAccountABI(llvm::StringRef Name);

/// Fields of the serialized input buffer's header. Both serializations begin
/// the same way, so this is not versioned.
enum class InputField : uint8_t {
#define SBF_INPUT_FIELD(ID, NAME, OFFSET, SIZE) ID,
#include "neverd/sbf/solana/SBFAccountLayout.def"
};

/// Everything an account entry can say, across both serializations. A name
/// appearing here does not mean every serialization places it at a fixed
/// offset: the unaligned form writes three of these after the account's data,
/// where the offset depends on how long that data is.
enum class AccountField : uint8_t {
#define SBF_ACCOUNT_FIELD_ID(ID, NAME, SUMMARY) ID,
#include "neverd/sbf/solana/SBFAccountLayout.def"
};

struct AccountFieldInfo {
  AccountField ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<AccountFieldInfo> accountFieldNames();
const AccountFieldInfo &getAccountFieldName(AccountField Field);

struct LayoutFieldInfo {
  llvm::StringLiteral Name;
  uint64_t Offset;
  /// Zero when the serialized data determines the length.
  uint64_t Size;
};

llvm::ArrayRef<LayoutFieldInfo> inputFieldInfos();
const LayoutFieldInfo &getInputFieldInfo(InputField Field);

/// Where one field sits under one serialization. The serialization is part of
/// the answer, not context a caller can drop: the same offset names a
/// different field under the other one.
struct AccountLayoutInfo {
  AccountField Field;
  llvm::StringLiteral Name;
  uint64_t Offset;
  /// Zero when the serialized data determines the length.
  uint64_t Size;
};

/// The fixed fields \p ABI places, in offset order.
llvm::ArrayRef<AccountLayoutInfo> accountFieldInfos(AccountABI ABI);

/// Where \p Field sits under \p ABI, or null when that serialization does not
/// place it at a fixed offset.
const AccountLayoutInfo *getAccountFieldInfo(AccountABI ABI,
                                             AccountField Field);

/// The field \p Offset lands in under \p ABI, or null when it lands past the
/// fixed part.
const AccountLayoutInfo *accountFieldAt(AccountABI ABI, uint64_t Offset);

/// Byte offset at which the first account entry starts, which is the end of the
/// input header.
uint64_t firstAccountOffset();

/// Total size of an account entry's fixed part under \p ABI.
uint64_t accountFixedSize(AccountABI ABI);

/// Report a gap or overlap between adjacent fixed fields in the layout tables.
llvm::Error validateAccountLayout();

} // namespace neverd::sbf

#endif // NEVERD_SBF_SOLANA_SBFACCOUNTLAYOUT_H
