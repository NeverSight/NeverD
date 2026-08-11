//===- AccountLayout.cpp - The buffer the loader hands a program --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/AccountLayout.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"

#include <array>
#include <vector>

namespace neverd::sbf {
namespace {

llvm::Error layoutError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      ("sbf: account layout: " + Message).str(),
      llvm::inconvertibleErrorCode());
}

/// Report the first place where \p Fields stop tiling their span. A field of
/// size zero ends the fixed part, so nothing may follow it.
template <typename FieldRange>
llvm::Error checkTiling(llvm::StringRef Table, const FieldRange &Fields) {
  uint64_t Expected = 0;
  for (auto [Index, Field] : llvm::enumerate(Fields)) {
    if (Field.Offset != Expected)
      return layoutError(Table + " field '" + Field.Name + "' starts at " +
                         llvm::Twine(Field.Offset) +
                         " but the previous field "
                         "ends at " +
                         llvm::Twine(Expected));
    if (Field.Size == 0 && Index + 1 != Fields.size())
      return layoutError(Table + " field '" + Field.Name +
                         "' has a data-determined length but is not last");
    Expected = Field.Offset + Field.Size;
  }
  return llvm::Error::success();
}

/// Offset of the first field whose length the data determines, or the end of
/// the range when every field is fixed.
template <typename FieldRange> uint64_t fixedSpan(const FieldRange &Fields) {
  uint64_t End = 0;
  for (const auto &Field : Fields) {
    if (Field.Size == 0)
      return Field.Offset;
    End = Field.Offset + Field.Size;
  }
  return End;
}

} // namespace

llvm::ArrayRef<AccountABIInfo> accountABIInfos() {
  static const std::array Table = {
#define SBF_ACCOUNT_ABI(ID, NAME, ALIGNED, DUPLICATE_ENTRY_SIZE, SUMMARY)      \
  AccountABIInfo{AccountABI::ID, NAME, (ALIGNED), (DUPLICATE_ENTRY_SIZE),      \
                 SUMMARY},
#include "neverd/sbf/SBFAccountLayout.def"
  };
  return Table;
}

const AccountABIInfo &getAccountABIInfo(AccountABI ABI) {
  return accountABIInfos()[static_cast<size_t>(ABI)];
}

llvm::StringRef accountABIName(AccountABI ABI) {
  return getAccountABIInfo(ABI).Name;
}

std::optional<AccountABI> parseAccountABI(llvm::StringRef Name) {
  for (const AccountABIInfo &Info : accountABIInfos())
    if (Info.Name == Name)
      return Info.ID;
  return std::nullopt;
}

llvm::ArrayRef<AccountFieldInfo> accountFieldNames() {
  static const std::array Table = {
#define SBF_ACCOUNT_FIELD_ID(ID, NAME, SUMMARY)                                \
  AccountFieldInfo{AccountField::ID, NAME, SUMMARY},
#include "neverd/sbf/SBFAccountLayout.def"
  };
  return Table;
}

const AccountFieldInfo &getAccountFieldName(AccountField Field) {
  return accountFieldNames()[static_cast<size_t>(Field)];
}

llvm::ArrayRef<LayoutFieldInfo> inputFieldInfos() {
  static const std::array Table = {
#define SBF_INPUT_FIELD(ID, NAME, OFFSET, SIZE)                                \
  LayoutFieldInfo{NAME, OFFSET, SIZE},
#include "neverd/sbf/SBFAccountLayout.def"
  };
  return Table;
}

const LayoutFieldInfo &getInputFieldInfo(InputField Field) {
  return inputFieldInfos()[static_cast<size_t>(Field)];
}

llvm::ArrayRef<AccountLayoutInfo> accountFieldInfos(AccountABI ABI) {
  // The flat table pairs a serialization with a placement, which is the shape
  // the .def can state without repeating a field's name once per ABI. Grouping
  // it by serialization once is what lets every caller walk one layout without
  // re-filtering.
  static const std::vector<std::vector<AccountLayoutInfo>> Layouts = [] {
    static const std::array Placements = {
#define SBF_ACCOUNT_FIELD(ABI, ID, OFFSET, SIZE)                               \
  AccountLayoutInfo{AccountField::ID, "", (OFFSET), (SIZE)},
#include "neverd/sbf/SBFAccountLayout.def"
    };
    static const std::array Owners = {
#define SBF_ACCOUNT_FIELD(ABI, ID, OFFSET, SIZE) AccountABI::ABI,
#include "neverd/sbf/SBFAccountLayout.def"
    };
    std::vector<std::vector<AccountLayoutInfo>> Grouped(
        accountABIInfos().size());
    for (auto [Index, Placement] : llvm::enumerate(Placements)) {
      AccountLayoutInfo Info = Placement;
      Info.Name = getAccountFieldName(Info.Field).Name;
      Grouped[static_cast<size_t>(Owners[Index])].push_back(Info);
    }
    return Grouped;
  }();
  return Layouts[static_cast<size_t>(ABI)];
}

const AccountLayoutInfo *getAccountFieldInfo(AccountABI ABI,
                                             AccountField Field) {
  for (const AccountLayoutInfo &Info : accountFieldInfos(ABI))
    if (Info.Field == Field)
      return &Info;
  return nullptr;
}

const AccountLayoutInfo *accountFieldAt(AccountABI ABI, uint64_t Offset) {
  for (const AccountLayoutInfo &Info : accountFieldInfos(ABI)) {
    // The last field's length is whatever the data says, so anything from its
    // start onwards is inside it.
    if (Info.Size == 0)
      return Offset >= Info.Offset ? &Info : nullptr;
    if (Offset >= Info.Offset && Offset < Info.Offset + Info.Size)
      return &Info;
  }
  return nullptr;
}

uint64_t firstAccountOffset() { return fixedSpan(inputFieldInfos()); }

uint64_t accountFixedSize(AccountABI ABI) {
  return fixedSpan(accountFieldInfos(ABI));
}

llvm::Error validateAccountLayout() {
  if (llvm::Error E = checkTiling("input", inputFieldInfos()))
    return E;
  for (const AccountABIInfo &ABI : accountABIInfos())
    if (llvm::Error E = checkTiling(ABI.Name, accountFieldInfos(ABI.ID)))
      return E;
  return llvm::Error::success();
}


} // namespace neverd::sbf
