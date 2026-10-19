//===- DriverProcessInventoryJSON.cpp - Explicit process observations
//------===//
///
/// \file
/// Explicit process observations.
///

#include "DriverProcessInventoryJSON.h"

#include "DriverProcessMemoryJSON.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <limits>
#include <type_traits>
namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "process_inventory: " + Text);
}
template <typename T> llvm::Error number(const llvm::json::Value &V, T &Out) {
  if constexpr (std::is_signed_v<T>) {
    auto N = V.getAsInteger();
    if (!N || *N < std::numeric_limits<T>::min() ||
        *N > std::numeric_limits<T>::max())
      return invalid("integer field is out of range");
    Out = T(*N);
  } else {
    auto N = V.getAsUINT64();
    if (!N || *N > std::numeric_limits<T>::max())
      return invalid("unsigned integer field is out of range");
    Out = T(*N);
  }
  return llvm::Error::success();
}
} // namespace
llvm::Expected<DriverProcessInventory>
parseDriverProcessInventory(const llvm::json::Value &V) {
  const auto *Array = V.getAsArray();
  if (!Array || Array->size() > MaxDriverProcessInventory)
    return invalid("requires a bounded array");
  DriverProcessInventory Out;
  for (const auto &Entry : *Array) {
    const auto *Object = Entry.getAsObject();
    if (!Object)
      return invalid("each record must be an object");
    DriverProcessInformation P;
    bool PID = false, Name = false;
    for (const auto &[Key, Value] : *Object) {
      if (Key == "process_id") {
        if (auto E = number(Value, P.ProcessID))
          return std::move(E);
        PID = true;
      } else if (Key == "parent_id") {
        if (auto E = number(Value, P.ParentID))
          return std::move(E);
      } else if (Key == "image_name") {
        auto Text = Value.getAsString();
        if (!Text)
          return invalid("image_name must be a string");
        P.ImageName = Text->str();
        Name = true;
      } else if (Key == "borrowed_thread_start") {
        uint64_t N;
        if (auto E = number(Value, N))
          return std::move(E);
        P.BorrowedThreadStart = N;
      } else if (Key == "borrowed_thread_system_start") {
        uint64_t N;
        if (auto E = number(Value, N))
          return std::move(E);
        P.BorrowedThreadSystemStart = N;
      } else if (Key == "images") {
        auto Images = parseDriverProcessImages(Value);
        if (!Images)
          return Images.takeError();
        P.Images = std::move(*Images);
      } else if (Key == "memory") {
        auto Regions = parseDriverProcessMemory(Value);
        if (!Regions)
          return Regions.takeError();
        P.Memory = std::move(*Regions);
      } else if (Key == "section_base_address") {
        uint64_t N;
        if (auto E = number(Value, N))
          return std::move(E);
        P.SectionBaseAddress = N;
      } else if (Key == "peb_address") {
        uint64_t N;
        if (auto E = number(Value, N))
          return std::move(E);
        P.PebAddress = N;
      } else if (Key == "image_file_name_hex") {
        auto Text = Value.getAsString();
        std::string Bytes;
        if (!Text || Text->size() != 2 * DriverProcessImageFileNameSize ||
            !llvm::tryGetFromHex(*Text, Bytes))
          return invalid("image_file_name_hex requires exactly 15 bytes");
        P.ImageFileName.emplace();
        std::copy(Bytes.begin(), Bytes.end(), P.ImageFileName->begin());
      }
#define NEVERD_PROCESS_COUNTER(Member, JSON, Type, Default, Offset, Width)     \
  else if (Key == JSON) {                                                      \
    if (auto E = number(Value, P.Member))                                      \
      return std::move(E);                                                     \
  }
#include "neverd/emulation/DriverProcessInventory.def"
#undef NEVERD_PROCESS_COUNTER
      else
        return invalid("unknown field " + Key.str());
    }
    if (!PID || !Name)
      return invalid("each record requires process_id and image_name");
    Out.push_back(std::move(P));
  }
  return Out;
}
llvm::json::Value
driverProcessInventoryJSON(const DriverProcessInventory &Inventory) {
  llvm::json::Array Out;
  for (const auto &P : Inventory) {
    llvm::json::Object O{{"process_id", P.ProcessID},
                         {"parent_id", P.ParentID},
                         {"image_name", P.ImageName}};
    if (P.BorrowedThreadStart)
      O["borrowed_thread_start"] = *P.BorrowedThreadStart;
    if (P.BorrowedThreadSystemStart)
      O["borrowed_thread_system_start"] = *P.BorrowedThreadSystemStart;
    if (P.SectionBaseAddress)
      O["section_base_address"] = *P.SectionBaseAddress;
    if (P.PebAddress)
      O["peb_address"] = *P.PebAddress;
    if (P.Memory)
      O["memory"] = driverProcessMemoryJSON(*P.Memory);
    if (!P.Images.empty())
      O["images"] = driverProcessImagesJSON(P.Images);
    if (P.ImageFileName)
      O["image_file_name_hex"] =
          llvm::toHex(llvm::ArrayRef(*P.ImageFileName), true);
#define NEVERD_PROCESS_COUNTER(Member, JSON, Type, Default, Offset, Width)     \
  O[JSON] = P.Member;
#include "neverd/emulation/DriverProcessInventory.def"
#undef NEVERD_PROCESS_COUNTER
    Out.push_back(std::move(O));
  }
  return Out;
}
} // namespace neverd::emulation
