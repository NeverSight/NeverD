//===- KernelModelProcessPeb.cpp - Explicit process environment pointers
//---===//
// NeverD Decompiler
///
/// \file
/// Explicit process environment pointers.
///

#include "KernelModel.h"
#include "KernelReferenceImage.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
namespace neverd::emulation {
llvm::Expected<uint64_t> KernelModel::processPeb(uint64_t Object) {
  auto Unsupported = [](const llvm::Twine &Text) -> llvm::Error {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "PsGetProcessPeb: " + Text);
  };
  const auto Identity = ProcessObjectIDs.find(Object);
  if (Identity == ProcessObjectIDs.end())
    return Unsupported("requires a known process object");
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return Unsupported("requires Windows 10.0.19041");
  std::optional<uint64_t> Pointer;
  if (Identity->second == windows::SystemProcessID)
    Pointer = 0;
  else if (ProcessInventory) {
    auto Facts = std::find_if(
        ProcessInventory->begin(), ProcessInventory->end(),
        [&](const auto &P) { return P.ProcessID == Identity->second; });
    if (Facts != ProcessInventory->end())
      Pointer = Facts->PebAddress;
  }
  if (!Pointer)
    return Unsupported("requires explicit peb_address");
  constexpr auto Offset = windows::ProcessPeb19041Offset;
  if (ProcessObjectSize < Offset + profile::PointerSize)
    return Unsupported("process layout does not contain the native PEB field");
  if (DebuggerReference) {
    const auto Getter = DebuggerReference->exports().find("PsGetProcessPeb");
    const uint8_t Body[] = {0x48, 0x8b, 0x81, 0x50, 0x05, 0x00, 0x00, 0xc3};
    if (Getter == DebuggerReference->exports().end() || !Getter->second ||
        !DebuggerReference->matchesCode(Getter->second, Body))
      return Unsupported(
          "reference getter does not match the selected native layout");
  }
  if (!PublishedProcessPebs.contains(Object)) {
    if (auto E = Memory.writeInteger(Object + Offset, *Pointer,
                                     profile::PointerSize))
      return E;
    PublishedProcessPebs.insert(Object);
  }
  // This publishes only the pointer. Process RAM, attachment and lifecycle
  // retain sole ownership of the user bytes and their permissions.
  return Memory.readInteger(Object + Offset, profile::PointerSize);
}
} // namespace neverd::emulation
