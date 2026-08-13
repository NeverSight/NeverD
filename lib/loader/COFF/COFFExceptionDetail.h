//===- COFFExceptionDetail.h - Private PE exception helpers -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_COFF_COFFEXCEPTIONDETAIL_H
#define NEVERD_LIB_LOADER_COFF_COFFEXCEPTIONDETAIL_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::coff_loader {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail {

inline constexpr uint32_t MaxLanguageRecords = 1u << 16;
inline constexpr unsigned MaxPersonalityVeneers = 8;

struct LanguageRecordBudget {
  uint64_t Remaining = MaxLanguageRecords;

  bool consume(uint64_t Count) {
    if (Count > Remaining)
      return false;
    Remaining -= Count;
    return true;
  }
};

void diagnose(ExceptionFunction &F, ExceptionParseStatus Status,
              llvm::StringRef Message);

bool addRVA(va_t ImageBase, uint32_t RVA, va_t &Address);

template <typename T>
inline std::optional<T> readScalar(const BinaryImage &Img, va_t Address) {
  const uint8_t *P = Img.readVA(Address, sizeof(T));
  if (!P)
    return std::nullopt;
  return readLE<T>(P);
}

bool readBytes(const BinaryImage &Img, va_t Address, size_t Size,
               const uint8_t *&Bytes);
bool isExecutableAddress(const BinaryImage &Img, va_t Address);
std::optional<ExceptionAddressRange>
checkedImageRange(va_t ImageBase, uint32_t BeginRVA, uint32_t EndRVA);
va_t normalizeTableCodeAddress(const BinaryImage &Img, va_t Address);
bool addCodeRVA(const BinaryImage &Img, uint32_t RVA, va_t &Address);
bool readCodeRVAField(const BinaryImage &Img, const uint8_t *P, va_t &Out);
std::optional<ExceptionAddressRange>
checkedCodeRange(const BinaryImage &Img, uint32_t BeginRVA, uint32_t EndRVA);
bool readRVAField(va_t ImageBase, const uint8_t *P, va_t &Out);

std::string directNameAt(const BinaryImage &Img, va_t Address);
std::optional<va_t> addSignedOffset(va_t Base, int64_t Offset);
std::pair<va_t, std::string> resolvePersonality(const BinaryImage &Img,
                                                va_t Start);
ExceptionPersonality classifyPersonality(llvm::StringRef Name);

bool isCoveredByRuntimeFunction(const BinaryImage &Img,
                                const ExceptionAddressRange &Range);
bool parseSEH(ExceptionFunction &F, const BinaryImage &Img);
bool parseFH3(ExceptionFunction &F, const BinaryImage &Img);
bool parseFH4(ExceptionFunction &F, const BinaryImage &Img);

bool parseGSCookie(ExceptionFunction &F, const BinaryImage &Img, va_t CookieVA);
std::optional<va_t> sehGSCookieAddress(const ExceptionFunction &F,
                                       const BinaryImage &Img);
void collectDirectCallTargets(const BinaryImage &Img, Arch A, va_t BodyVA,
                              const uint8_t *Code, size_t CodeSize,
                              std::vector<std::string> &Names);
std::optional<ExceptionPersonality>
inferGSPersonality(const ExceptionFunction &F, const BinaryImage &Img);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail
} // namespace neverd::coff_loader

#endif // NEVERD_LIB_LOADER_COFF_COFFEXCEPTIONDETAIL_H
