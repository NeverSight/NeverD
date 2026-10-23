//===- RegistrationJumpEH4.cpp - EH4 nonlocal cookie argument -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate the source cookie address carried by EH4 nonlocal captures.

#include "RegistrationJumpCapture.h"

#include "neverd/loader/BinaryImage.h"

namespace neverd::registration_state {

std::optional<va_t> checkedEH4JumpCookie(const BinaryImage &Image,
                                         const RegistrationChainInfo &Chain,
                                         const FrameState &Frame, int32_t SP) {
  if (!Image.DynInfo.SecurityCookieRVA || Image.Base > UINT32_MAX ||
      Image.DynInfo.SecurityCookieRVA > UINT32_MAX - Image.Base ||
      SP > INT32_MAX - 20)
    return std::nullopt;
  const va_t Cookie = Image.Base + Image.DynInfo.SecurityCookieRVA;
  const auto Table = Frame.load(-8, 4);
  if (Cookie > UINT32_MAX - 3 ||
      Frame.load(SP + 16, 4).preferredAddress() != Cookie ||
      !Table.SecurityCookie || Table.CookieFrameOffset ||
      Table.cookieAddress() != Chain.ScopeTableVA)
    return std::nullopt;
  return Cookie;
}

} // namespace neverd::registration_state
