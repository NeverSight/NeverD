//===- X87TranslationTestSupport.h - Authored x87 guest fixtures -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared deterministic fixtures for checked portable x87 translation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X87TRANSLATIONTESTSUPPORT_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X87TRANSLATIONTESTSUPPORT_H

#include "neverd/translate/X86/RuntimeX87Helpers.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace neverd::translate::test {

inline constexpr uint64_t Entry = 0x4000;
inline constexpr RegisterID X87ID = kFirstExtensionRegisterID + 23;
inline constexpr auto Profile = TranslationRuntimeProfileV1::PortableX87;

constexpr bool nativeLinuxAArch64() {
#if defined(__aarch64__) && defined(__linux__)
  return true;
#else
  return false;
#endif
}

inline std::vector<uint8_t> program() {
  return {0x48, 0xc7, 0xc0, 0,    0, 0, 0, // MOV RAX, 0.
          0xdb, 0x2f, 0xdb, 0x2e,          // FLD tbyte [RDI]; FLD tbyte [RSI].
          0xde, 0xc1, 0xdb, 0x3a,          // FADDP; FSTP tbyte [RDX].
          0xdf, 0xe0, 0xc3};               // FNSTSW AX; RET.
}

inline std::vector<uint8_t> controlProgram(bool Loop) {
  auto Code = program();
  if (Loop) {
    // FCHS; SUB RCX, 1; JNE back to FCHS. SUB belongs to the established
    // scalar translation subset; DEC is not admitted by that subset.
    const uint8_t Backedge[] = {0xd9, 0xe0, 0x48, 0x83, 0xe9, 1, 0x75, 0xf8};
    Code.insert(Code.begin() + 13, std::begin(Backedge), std::end(Backedge));
  } else {
    Code.pop_back();
    const uint8_t Branch[] = {0x48, 0x85, 0xc9, 0x74, 5,   0x48,
                              0x83, 0xc0, 1,    0xc3, 0xc3};
    Code.insert(Code.end(), std::begin(Branch), std::end(Branch));
  }
  return Code;
}

inline GuestState fixture(std::vector<uint8_t> Code = program(),
                          unsigned Top = 5, uint16_t CW = 0x37f) {
  auto State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  llvm::cantFail(setRegisterValue(State, 16, llvm::APInt(64, Entry)));
  llvm::cantFail(setRegisterValue(State, 7, llvm::APInt(64, 0x1000))); // RDI.
  llvm::cantFail(setRegisterValue(State, 6, llvm::APInt(64, 0x2000))); // RSI.
  llvm::cantFail(setRegisterValue(State, 2, llvm::APInt(64, 0x3001))); // RDX.
  llvm::cantFail(setRegisterValue(State, 4, llvm::APInt(64, 0x8000))); // RSP.
  State.Memory = {
      {0x1000,
       MemoryPermission::Read,
       0,
       {1, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x3f}},
      {0x2000,
       MemoryPermission::Read,
       0,
       {0, 0, 0, 0, 0, 0, 0, 0x80, 0xfe, 0x3f}},
      {0x3000, MemoryPermission::Write, 0, std::vector<uint8_t>(12, 0xa5)},
      {Entry, MemoryPermission::Read | MemoryPermission::Execute, 1,
       std::move(Code)},
      {0x8000, MemoryPermission::Read, 0, {0x34, 0x12, 0, 0, 0, 0, 0, 0}},
  };
  PortableX87StateV1 X87;
  X87.EnvironmentKnown = 1;
  llvm::support::endian::write16le(X87.Image.data(), CW);
  llvm::support::endian::write16le(X87.Image.data() + 2, Top << 11);
  llvm::cantFail(setPortableX87GuestStateV1(State, X87ID, X87));
  return State;
}

inline TranslationOptions options(bool Native = false) {
  TranslationOptions Options;
  Options.RuntimeProfile = Profile;
  Options.RequiredCapabilities =
      TranslationCapability::ScalarInteger | TranslationCapability::X87;
  Options.BlockCache = BlockCachePolicy::Disabled;
  Options.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  if (!Native) {
    Options.Mode = TranslationMode::AOT;
    Options.Target.Kind = HostTargetKind::Explicit;
    Options.Target.Architecture = GuestArchitecture::AArch64;
    Options.Target.Triple = "aarch64-unknown-linux-gnu";
  }
  return Options;
}

} // namespace neverd::translate::test

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X87TRANSLATIONTESTSUPPORT_H
