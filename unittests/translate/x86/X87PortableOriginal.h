//===- X87PortableOriginal.h - Native reference for owned bytes -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute only this test's authored x64 byte vector under an explicit x87
/// image. Runtime-provided and imported programs never enter this helper.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X87PORTABLEORIGINAL_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X87PORTABLEORIGINAL_H

#include "neverd/translate/X86/X87PortableRuntime.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Memory.h"

#include <array>
#include <cstring>
#include <memory>
#include <optional>

namespace neverd::test {

struct X87OriginalObservation {
  alignas(16) std::array<uint8_t, 512> Image{};
  std::array<uint8_t, 12> Memory{};
  uint64_t Return = 0;
};

class X87PortableOriginal {
  llvm::sys::MemoryBlock Code;
  explicit X87PortableOriginal(llvm::sys::MemoryBlock Code) : Code(Code) {}

public:
  X87PortableOriginal(const X87PortableOriginal &) = delete;
  X87PortableOriginal &operator=(const X87PortableOriginal &) = delete;
  ~X87PortableOriginal() {
    if (Code.base())
      (void)llvm::sys::Memory::releaseMappedMemory(Code);
  }
  static llvm::Expected<std::unique_ptr<X87PortableOriginal>>
  create(llvm::ArrayRef<uint8_t> Bytes) {
#if defined(__x86_64__) && !defined(_WIN32)
    std::error_code Error;
    auto Code = llvm::sys::Memory::allocateMappedMemory(
        Bytes.size(), nullptr,
        llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, Error);
    if (Error)
      return llvm::errorCodeToError(Error);
    auto Owner =
        std::unique_ptr<X87PortableOriginal>(new X87PortableOriginal(Code));
    std::memcpy(Code.base(), Bytes.data(), Bytes.size());
    if (auto Error = llvm::sys::Memory::protectMappedMemory(
            Code, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC))
      return llvm::errorCodeToError(Error);
    return Owner;
#else
    return std::unique_ptr<X87PortableOriginal>();
#endif
  }

  X87OriginalObservation run(const translate::PortableX87StateV1 &Initial,
                             unsigned Shape, uint64_t Count,
                             const std::array<uint8_t, 10> &A,
                             const std::array<uint8_t, 10> &B) const {
    X87OriginalObservation Result;
#if defined(__x86_64__) && !defined(_WIN32)
    alignas(16) std::array<uint8_t, 512> Host;
    __asm__ volatile("fxsave64 %0" : "=m"(Host) : : "memory");
    Result.Image = Initial.Image;
    // The reference changes x87 state only. Keep the caller's MXCSR and XMM
    // environment, as the native x87 semantic owner does.
    std::memcpy(Result.Image.data() + 24, Host.data() + 24, 8);
    std::memcpy(Result.Image.data() + 160, Host.data() + 160, 352);
    Result.Memory.fill(0xa5);
    __asm__ volatile("fxrstor64 %0"
                     :
                     : "m"(Result.Image)
                     : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)",
                       "st(5)", "st(6)", "st(7)", "xmm0", "xmm1", "xmm2",
                       "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",
                       "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    using Leaf = uint64_t (*)(const void *, const void *, void *);
    using Control = uint64_t (*)(const void *, const void *, void *, uint64_t);
    Result.Return =
        Shape == 0 ? reinterpret_cast<Leaf>(Code.base())(
                         A.data(), B.data(), Result.Memory.data() + 1)
                   : reinterpret_cast<Control>(Code.base())(
                         A.data(), B.data(), Result.Memory.data() + 1, Count);
    __asm__ volatile("fxsave64 %0\n\tfxrstor64 %1"
                     : "+m"(Result.Image)
                     : "m"(Host)
                     : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)",
                       "st(5)", "st(6)", "st(7)", "xmm0", "xmm1", "xmm2",
                       "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",
                       "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
    return Result;
  }
};

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X87PORTABLEORIGINAL_H
