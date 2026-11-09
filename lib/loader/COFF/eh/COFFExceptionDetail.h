//===- COFFExceptionDetail.h - Private PE exception helpers -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation detail of the PE exception-table decoders.  The bounds and
/// helpers here are shared between the translation units under
/// `lib/loader/COFF/eh` and are not part of any public interface; nothing
/// outside that directory may include this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_COFF_EH_COFFEXCEPTIONDETAIL_H
#define NEVERD_LIB_LOADER_COFF_EH_COFFEXCEPTIONDETAIL_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd::coff_loader {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail {

struct LanguageRecordBudget {
  uint64_t Remaining =
      ::neverd::limits::loader::kCOFFExceptionDetailMaxLanguageRecords;

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

using CxxFuncInfoGroups =
    std::unordered_map<uint32_t, std::vector<ExceptionAddressRange>>;
using PrimaryFunctionByBegin =
    std::unordered_map<va_t, const ExceptionFunction *>;

CxxFuncInfoGroups buildCxxFuncInfoGroups(const BinaryImage &Img);
/// The FuncInfo3 RVA the C++ handler data at \p HandlerDataVA names, when
/// the record there carries an FH3 magic number.
std::optional<uint32_t> fh3FuncInfoRVA(const BinaryImage &Img,
                                       va_t HandlerDataVA);
/// Where the code of each x64 pdata record starts whose primary unwind
/// information installs a handler with data naming one of \p FuncInfoRVAs:
/// the functions of those FuncInfo groups, materialized or not.
std::vector<va_t> fh3GroupRecordBegins(const BinaryImage &Img,
                                       const std::set<uint32_t> &FuncInfoRVAs);
PrimaryFunctionByBegin indexPrimaryFunctionsByBegin(const BinaryImage &Img);
bool parseFH3(ExceptionFunction &F, const BinaryImage &Img,
              const CxxFuncInfoGroups *Groups = nullptr);
bool parseFH4(ExceptionFunction &F, const BinaryImage &Img);

bool parseGSCookie(ExceptionFunction &F, const BinaryImage &Img, va_t CookieVA);
std::optional<va_t> sehGSCookieAddress(const ExceptionFunction &F,
                                       const BinaryImage &Img);
bool collectDirectCallTargets(const BinaryImage &Img, Arch A, va_t BodyVA,
                              const uint8_t *Code, size_t CodeSize,
                              std::vector<std::string> &Names);
/// The base personality a stripped GS wrapper at \p PersonalityVA delegates
/// to -- C_specific_handler, CxxFrameHandler3 or CxxFrameHandler4 -- as the
/// wrapper's own calls name it, or std::nullopt when the routine there is no
/// such wrapper.  It depends on the routine alone, so a caller may keep it per
/// address.
std::optional<ExceptionPersonality> gsWrapperBasePersonality(
    const BinaryImage &Img, va_t PersonalityVA,
    const PrimaryFunctionByBegin *PrimaryByBegin = nullptr);
/// \p F's GS personality when its handler data is valid for a wrapper whose
/// base personality is \p BasePersonality.
std::optional<ExceptionPersonality>
inferGSPersonality(const ExceptionFunction &F, const BinaryImage &Img,
                   ExceptionPersonality BasePersonality);
/// Both: the wrapper at \p F's handler, and \p F's handler data for it.
std::optional<ExceptionPersonality>
inferGSPersonality(const ExceptionFunction &F, const BinaryImage &Img,
                   const PrimaryFunctionByBegin *PrimaryByBegin = nullptr);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail
} // namespace neverd::coff_loader

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::coff_loader {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail {
inline constexpr uint32_t MaxLanguageRecords =
    ::neverd::limits::loader::kCOFFExceptionDetailMaxLanguageRecords;
inline constexpr unsigned MaxPersonalityVeneers =
    ::neverd::limits::loader::kCOFFExceptionDetailMaxPersonalityVeneers;
} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail
} // namespace neverd::coff_loader

#endif // NEVERD_LIB_LOADER_COFF_EH_COFFEXCEPTIONDETAIL_H
