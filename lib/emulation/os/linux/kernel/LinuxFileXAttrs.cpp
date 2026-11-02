//===- LinuxFileXAttrs.cpp - Observed extended attribute query failures --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxKernelAvailability.h"

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxFiles::attributeNameError(uint64_t Address) {
  for (uint64_t I = 0; I < XAttrNameLimit; ++I) {
    if (Address >= Layout.UserLimit || I >= Layout.UserLimit - Address)
      return std::optional<uint64_t>(uint64_t(0) - BadAddress);
    auto Access = CPU.canAccess(Address + I, 1, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return std::optional<uint64_t>(uint64_t(0) - BadAddress);
    auto Byte = CPU.readInteger(Address + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte)
      return I ? std::optional<uint64_t>()
               : std::optional<uint64_t>(uint64_t(0) - RangeError);
  }
  return std::optional<uint64_t>(uint64_t(0) - RangeError);
}

llvm::Expected<std::optional<uint64_t>> LinuxFiles::extendedAttribute(
    ServiceKind Kind, const ProcessServiceEvent &Event,
    const std::optional<LinuxKernelOptions> &Kernel, ProcessResult &Result) {
  if (!Options)
    return unsupported(Result, FileInputsMissing);
  const auto Order = Kernel ? xattrQueryImport(*Kernel) : std::nullopt;
  if (!Order)
    return unsupported(Result, XAttrKernel);
  const auto &[Target, Name, Value, Size, A4, A5] = Event.Arguments;
  const bool NameFirst = *Order == LinuxXAttrQueryImport::NameBeforeTarget;
  if (NameFirst) {
    auto Error = attributeNameError(Name);
    if (!Error)
      return Error.takeError();
    if (*Error)
      return *Error;
  }
  if (Kind == ServiceKind::FGetXAttr) {
    if (!Descriptors.count(uint32_t(Target)))
      return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  } else {
    auto Imported = readPath(Target);
    if (!Imported)
      return Imported.takeError();
    if (const auto *Error = std::get_if<uint32_t>(&*Imported))
      return std::optional<uint64_t>(uint64_t(0) - *Error);
    auto Path = parsePath(std::get<std::string>(*Imported));
    if (!Path)
      return unsupported(Result, FilePathForm);
    switch (lookupPath(Path->Name, Path->RequiresDirectory)) {
    case PathKind::SymbolicLink:
    case PathKind::LinkTraversal:
      return unsupported(Result, FileLinkTraversal);
    case PathKind::UnmodeledProc:
      return unsupported(Result, "proc entry has no modeled semantics");
    case PathKind::Missing:
      return std::optional<uint64_t>(uint64_t(0) - NoEntry);
    case PathKind::NotDirectory:
      return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
    case PathKind::ProcFile:
    case PathKind::File:
    case PathKind::Device:
    case PathKind::Directory:
      break;
    }
  }
  if (!NameFirst) {
    auto Error = attributeNameError(Name);
    if (!Error)
      return Error.takeError();
    if (*Error)
      return *Error;
  }
  // Contents and stat observations do not supply xattrs or namespace policy.
  // Resolve known import/target errors without touching the value buffer.
  return unsupported(Result, XAttrObservation);
}
} // namespace neverd::emulation::linux_model
