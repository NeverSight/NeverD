//===- DriverFiniteFileSystemJSON.cpp - Validated finite volume inventories
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validated finite volume inventories.
///

#include "DriverFiniteFileSystemJSON.h"

#include "../WindowsUnicode.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <map>
#include <set>

namespace neverd::emulation {
llvm::Error validateDriverFilterManager(const DriverOptions &O) {
  if (!O.FilterManagerFileNames)
    return llvm::Error::success();
  if (!O.Version || O.Version->Major != 10 || O.Version->Minor != 0 ||
      O.Version->Build != 19041 ||
      (*O.FilterManagerFileNames && !O.FiniteFileSystem))
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "filter_manager_file_names requires windows_version 10.0.19041 and an "
        "explicit filesystem when enabled");
  return llvm::Error::success();
}
namespace {
llvm::Error invalid() {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "invalid or oversized filesystem inventory");
}
bool path(llvm::StringRef S) {
  auto Units = windows_unicode::utf16(S);
  if (!Units) {
    llvm::consumeError(Units.takeError());
    return false;
  }
  if (S.empty() || Units->size() > 240 || S.front() != '\\' || S.back() == '\\')
    return false;
  for (uint16_t C : *Units)
    if (C < 32 || C == 127 || C == '/' || C == ':' || C == '*' || C == '?')
      return false;
  while (!S.empty()) {
    S = S.drop_front();
    auto Component = S.take_until([](char C) { return C == '\\'; });
    if (Component.empty() || Component == "." || Component == "..")
      return false;
    S = S.drop_front(Component.size());
  }
  return true;
}
bool fields(const llvm::json::Object &O,
            std::initializer_list<llvm::StringRef> Allowed) {
  for (const auto &KV : O) {
    bool Found = false;
    for (auto A : Allowed)
      Found |= llvm::StringRef(KV.first) == A;
    if (!Found)
      return false;
  }
  return true;
}
} // namespace

bool validDriverFiniteFileSystemPath(llvm::StringRef Path, bool AllowRoot) {
  return (AllowRoot && Path == "\\") || path(Path);
}

llvm::Error validateDriverFiniteFileSystem(
    const std::optional<std::vector<DriverFiniteFileSystemVolume>> &Volumes) {
  if (!Volumes)
    return llvm::Error::success();
  if (Volumes->size() > 26)
    return invalid();
  size_t Count = 0, Bytes = 0;
  std::set<std::string> Drives, Devices, Aliases;
  for (const auto &V : *Volumes) {
    if (auto E = windows_unicode::validateUpcase(V.UpcaseTable))
      return E;
    if (V.Drive.size() != 1 || V.Drive[0] < 'A' || V.Drive[0] > 'Z' ||
        !Drives.insert(V.Drive).second || !path(V.DeviceName) ||
        !llvm::all_of(V.DeviceName, [](char C) { return llvm::isASCII(C); }) ||
        !llvm::StringRef(V.DeviceName).starts_with_insensitive("\\Device\\") ||
        llvm::StringRef(V.DeviceName).drop_front(8).contains('\\') ||
        !Devices.insert(llvm::StringRef(V.DeviceName).lower()).second)
      return invalid();
    std::map<std::string, bool> Names;
    Names.emplace("", true);
    for (const auto &E : V.Entries) {
      Count++;
      Bytes += E.Data.size();
      auto Key = windows_unicode::key(E.Path, V.UpcaseTable);
      if (!Key)
        return Key.takeError();
      if (Count > MaxDriverFiniteFileSystemEntries ||
          Bytes > MaxDriverFiniteFileSystemBytes || !path(E.Path) ||
          (E.Directory && !E.Data.empty()) ||
          !Names.emplace(*Key, E.Directory).second)
        return invalid();
    }
    for (const auto &E : V.Entries) {
      auto Parent = windows_unicode::key(
          llvm::StringRef(E.Path).take_front(E.Path.rfind('\\')),
          V.UpcaseTable);
      if (!Parent)
        return Parent.takeError();
      auto It = Names.find(*Parent);
      if (It == Names.end() || !It->second)
        return invalid();
    }
    if (V.Aliases.size() > 32)
      return invalid();
    for (const auto &Alias : V.Aliases) {
      const auto Key = llvm::StringRef(Alias.Name).lower();
      if (!path(Alias.Name) ||
          !llvm::all_of(Alias.Name, [](char C) { return llvm::isASCII(C); }) ||
          llvm::StringRef(Alias.Name).drop_front().contains('\\') ||
          Key == "\\device" || Key == "\\dosdevices" || Key == "\\registry" ||
          Key == "\\driver" || Key == "\\callback" ||
          !Aliases.insert(Key).second ||
          (Alias.Path != "\\" && !path(Alias.Path)))
        return invalid();
      auto KeyPath = windows_unicode::key(Alias.Path == "\\" ? "" : Alias.Path,
                                          V.UpcaseTable);
      if (!KeyPath)
        return KeyPath.takeError();
      auto It = Names.find(*KeyPath);
      if (It == Names.end() || !It->second)
        return invalid();
    }
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<DriverFiniteFileSystemVolume>>
parseDriverFiniteFileSystem(const llvm::json::Value &Value) {
  const auto *A = Value.getAsArray();
  if (!A || A->size() > 26)
    return invalid();
  std::vector<DriverFiniteFileSystemVolume> Result;
  size_t TotalBytes = 0, TotalEntries = 0;
  for (const auto &Item : *A) {
    const auto *O = Item.getAsObject();
    if (!O || !fields(*O, {"drive", "device_name", "entries", "read_only",
                           "aliases", "upcase_table"}))
      return invalid();
    auto Drive = O->getString("drive"), Device = O->getString("device_name");
    const auto *Entries = O->getArray("entries");
    if (!Drive || !Device || !Entries || Entries->size() > 256)
      return invalid();
    DriverFiniteFileSystemVolume V{Drive->str(), Device->str(), {}};
    if (const auto *Value = O->get("upcase_table")) {
      auto Hex = Value->getAsString();
      if (!Hex || Hex->size() != 65536 * 4 ||
          !llvm::all_of(*Hex, llvm::isHexDigit))
        return invalid();
      auto Bytes = llvm::fromHex(*Hex);
      V.UpcaseTable.resize(65536);
      for (size_t I = 0; I < V.UpcaseTable.size(); ++I)
        V.UpcaseTable[I] =
            llvm::support::endian::read16le(Bytes.data() + I * 2);
    }
    if (O->get("read_only")) {
      auto ReadOnly = O->getBoolean("read_only");
      if (!ReadOnly)
        return invalid();
      V.ReadOnly = *ReadOnly;
    }
    if (const auto *Value = O->get("aliases")) {
      const auto *List = Value->getAsArray();
      if (!List || List->size() > 32)
        return invalid();
      for (const auto &Item : *List) {
        const auto *A = Item.getAsObject();
        if (!A || !fields(*A, {"name", "path"}))
          return invalid();
        auto Name = A->getString("name"), Path = A->getString("path");
        if (!Name || !Path)
          return invalid();
        V.Aliases.push_back({Name->str(), Path->str()});
      }
    }
    for (const auto &Item : *Entries) {
      const auto *E = Item.getAsObject();
      if (!E || !fields(*E, {"path", "directory", "data"}))
        return invalid();
      auto Path = E->getString("path");
      auto Directory = E->getBoolean("directory");
      auto Data = E->getString("data");
      if (!Path || !Directory || !Data || Data->size() > 32 * 1024 * 1024 ||
          Data->size() % 2 || !llvm::all_of(*Data, llvm::isHexDigit))
        return invalid();
      TotalBytes += Data->size() / 2;
      if (++TotalEntries > MaxDriverFiniteFileSystemEntries ||
          TotalBytes > MaxDriverFiniteFileSystemBytes)
        return invalid();
      auto Bytes = llvm::fromHex(*Data);
      V.Entries.push_back(
          {Path->str(), *Directory, {Bytes.begin(), Bytes.end()}});
    }
    Result.push_back(std::move(V));
  }
  if (auto E = validateDriverFiniteFileSystem(Result))
    return E;
  return Result;
}

llvm::json::Value driverFiniteFileSystemJSON(
    const std::optional<std::vector<DriverFiniteFileSystemVolume>> &Volumes) {
  if (!Volumes)
    return nullptr;
  llvm::json::Array A;
  for (const auto &V : *Volumes) {
    llvm::json::Array Entries;
    llvm::json::Array Aliases;
    for (const auto &Alias : V.Aliases)
      Aliases.push_back(
          llvm::json::Object{{"name", Alias.Name}, {"path", Alias.Path}});
    for (const auto &E : V.Entries)
      Entries.push_back(llvm::json::Object{{"path", E.Path},
                                           {"directory", E.Directory},
                                           {"data", llvm::toHex(E.Data)}});
    llvm::json::Object Volume{{"drive", V.Drive},
                              {"device_name", V.DeviceName},
                              {"entries", std::move(Entries)},
                              {"read_only", V.ReadOnly},
                              {"aliases", std::move(Aliases)}};
    if (!V.UpcaseTable.empty()) {
      std::vector<uint8_t> Bytes(V.UpcaseTable.size() * 2);
      for (size_t I = 0; I < V.UpcaseTable.size(); ++I)
        llvm::support::endian::write16le(Bytes.data() + I * 2,
                                         V.UpcaseTable[I]);
      Volume["upcase_table"] = llvm::toHex(Bytes);
    }
    A.push_back(std::move(Volume));
  }
  return A;
}
} // namespace neverd::emulation
