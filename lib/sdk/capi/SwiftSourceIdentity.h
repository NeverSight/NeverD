#ifndef NEVERD_SDK_CAPI_SWIFTSOURCEIDENTITY_H
#define NEVERD_SDK_CAPI_SWIFTSOURCEIDENTITY_H

#include "neverd/Common.h"

#include <string>
#include <utility>

namespace neverd::sdk::swift_source {
/// Mach-O contributes at most one underscore; retain the supplied spelling
/// in report rows while comparing the underlying native symbol identity.
inline std::string normalizedSourceSymbol(std::string Symbol) {
  if (Symbol.starts_with("_"))
    Symbol.erase(0, 1);
  return Symbol;
}
inline std::pair<va_t, std::string> sourceIdentity(va_t Entry,
                                                   std::string Symbol) {
  return {Entry, normalizedSourceSymbol(std::move(Symbol))};
}
} // namespace neverd::sdk::swift_source
#endif
