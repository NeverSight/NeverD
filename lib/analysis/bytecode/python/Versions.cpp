//===- Versions.cpp - CPython signatures and pyc header layouts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Select the code ABI from disk evidence or an explicit raw-code profile.
//===----------------------------------------------------------------------===//

#include "Artifacts.h"

#include "neverd/analysis/PythonVersions.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace neverd::python;

unsigned neverd::python::profileVersion(StringRef Profile) {
  if (!Profile.consume_front("cpython-"))
    return 0;
  for (const auto &R : Releases)
    if (Profile == R.Number)
      return R.Version;
  return 0;
}

unsigned neverd::python::pythonVersion(uint32_t Signature, unsigned Requested) {
  // 3.5.2 changed argument packing for BUILD_MAP_UNPACK_WITH_CALL.
  // Both encodings have the same instruction width and code-object layout.
  if (Signature == 0x0a0d0d16 && (!Requested || Requested == 305))
    return 305;
  for (const auto &R : Releases)
    if ((Signature == R.Signature || (R.Version >= 106 && R.Version <= 207 &&
                                      Signature == R.Signature + 1)) &&
        (!Requested || R.Version == Requested))
      return R.Version;
  return 0;
}

bool neverd::python::knownVersion(unsigned Version) {
  for (const auto &R : Releases)
    if (R.Version == Version)
      return true;
  return false;
}

std::string neverd::python::dialectName(unsigned Version) {
  for (const auto &R : Releases)
    if (R.Version == Version)
      return std::string("CPython ") + R.Number;
  llvm_unreachable("unknown CPython release");
}

size_t neverd::python::pycHeader(Job &J, json::Object &R, StringRef Bytes,
                                 unsigned &Version) {
  if (Bytes.size() < 4)
    malformed(0, "Invalid or truncated pyc magic");
  uint32_t Signature = support::endian::read32le(Bytes.data());
  unsigned Requested = profileVersion(J.Profile);
  Version = pythonVersion(Signature);
  if (!Version && Bytes.substr(2, 2) != "\r\n")
    malformed(0, "Invalid or truncated pyc magic");
  R["magic"] = Version && Version < 103 ? Signature : Signature & 0xffff;
  if (!Version)
    unsupported(0, "Unqualified CPython magic and code layout");
  // 1.1 and 1.2 shipped the same signature.
  bool Shared = Version == 101 && !Requested;
  if (Version == 101 && Requested == 102)
    Version = 102;
  if (Shared)
    R["version_candidates"] = json::Array{"1.1", "1.2"};
  size_t Size = Version >= 307 ? 16 : Version >= 303 ? 12 : 8;
  if (Bytes.size() < Size)
    malformed(Bytes.size(), "Truncated pyc header");
  unsigned Flags = Size == 16 ? support::endian::read32le(Bytes.data() + 4) : 0;
  if (Flags != 0 && Flags != 1 && Flags != 3)
    malformed(4, "Invalid pyc invalidation flags");
  R["header_mode"] = Flags == 0   ? "timestamp"
                     : Flags == 1 ? "unchecked_hash"
                                  : "checked_hash";
  if (Flags)
    R["source_hash"] = toHex(Bytes.substr(8, 8), true);
  else {
    const char *Stamp = Bytes.data() + (Size == 16 ? 8 : 4);
    R["timestamp"] = support::endian::read32le(Stamp);
    if (Size >= 12)
      R["source_size"] = support::endian::read32le(Stamp + 4);
  }
  if (Version >= 106 && Version <= 207)
    for (const auto &Release : Releases)
      if (Release.Version == Version)
        R["unicode_literals"] = Signature == Release.Signature + 1;
  R["optimization"] = "unknown";
  R["dialect"] = Shared ? "CPython 1.1/1.2" : dialectName(Version);
  return Size;
}
