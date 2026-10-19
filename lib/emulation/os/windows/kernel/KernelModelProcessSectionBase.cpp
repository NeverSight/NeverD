//===- KernelModelProcessSectionBase.cpp - Declared process image bases
//----===//
// NeverD Decompiler
///
/// \file
/// Declared process image bases.
///

#include "KernelModel.h"
#include "KernelReferenceImage.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
namespace neverd::emulation {
llvm::Expected<uint64_t>
KernelModel::processSectionBaseAddress(uint64_t Object) {
  auto Unsupported = [](const llvm::Twine &Text) -> llvm::Error {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "PsGetProcessSectionBaseAddress: " + Text);
  };
  const auto Identity = ProcessObjectIDs.find(Object);
  if (Identity == ProcessObjectIDs.end())
    return Unsupported("requires a known process object");
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041 || !ProcessInventory)
    return Unsupported("requires Windows 10.0.19041 and process_inventory");
  const auto Facts = std::find_if(
      ProcessInventory->begin(), ProcessInventory->end(),
      [&](const auto &P) { return P.ProcessID == Identity->second; });
  if (Facts == ProcessInventory->end() || !Facts->SectionBaseAddress)
    return Unsupported("requires explicit section_base_address");
  constexpr auto Offset = windows::ProcessSectionBase19041Offset;
  if (ProcessObjectSize < Offset + profile::PointerSize)
    return Unsupported("process layout does not contain the native base field");
  if (DebuggerReference) {
    const auto Getter =
        DebuggerReference->exports().find("PsGetProcessSectionBaseAddress");
    const uint8_t Body[] = {0x48, 0x8b, 0x81, 0x20, 0x05, 0x00, 0x00, 0xc3};
    if (Getter == DebuggerReference->exports().end() || !Getter->second ||
        !DebuggerReference->matchesCode(Getter->second, Body))
      return Unsupported(
          "reference getter does not match the selected native layout");
  }
  if (!PublishedProcessSectionBases.contains(Object)) {
    if (auto E = Memory.writeInteger(
            Object + Offset, *Facts->SectionBaseAddress, profile::PointerSize))
      return E;
    PublishedProcessSectionBases.insert(Object);
  }
  // Read the single published field; direct guest reads observe these bytes
  // too.
  return Memory.readInteger(Object + Offset, profile::PointerSize);
}
} // namespace neverd::emulation
