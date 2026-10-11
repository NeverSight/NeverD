//===- Endpoint.cpp - Private passive endpoint comparison keys ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// URL classification and fixed public method/header vocabularies.
///
//===----------------------------------------------------------------------===//

#include "../Internal.h"
#include "ada.h"

#include "neverd/web/Interfaces.h"

namespace neverd::web {
namespace {
std::string asciiLower(std::string_view Text) {
  std::string Result(Text);
  for (auto &C : Result)
    if (C >= 'A' && C <= 'Z')
      C += 'a' - 'A';
  return Result;
}
} // namespace

std::string interfaceMethod(std::string_view Method, bool Fetch) {
  if (Method.size() > 16)
    return "other";
  std::string Selected(Method);
  if (Fetch) {
    const auto Lower = asciiLower(Method);
    for (const auto *Known :
         {"delete", "get", "head", "options", "post", "put"})
      if (Lower == Known)
        for (auto &C : Selected)
          if (C >= 'a' && C <= 'z')
            C -= 'a' - 'A';
  }
  for (const auto *Known : {"DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT",
                            "PATCH", "CONNECT", "TRACE"})
    if (Selected == Known)
      return Selected;
  return "other";
}

std::string interfaceHeaderKind(std::string_view Name) {
  if (Name.size() > 64)
    return "other";
  const auto Lower = asciiLower(Name);
  for (const auto *Sensitive : {"authorization", "proxy-authorization",
                                "cookie", "set-cookie", "x-api-key"})
    if (Lower == Sensitive)
      return "sensitive";
  for (const auto *Known :
       {"accept", "accept-encoding", "accept-language", "cache-control",
        "content-encoding", "content-length", "content-type", "origin",
        "referer", "user-agent", "location", "etag", "if-none-match"})
    if (Lower == Known)
      return Lower;
  return "other";
}

InterfaceEndpoint inspectInterfaceEndpoint(std::string_view URL) {
  InterfaceEndpoint R;
  R.Status = "invalid_url";
  if (URL.size() > MaxInterfaceURLBytes)
    throw Error("interface_url_budget_exceeded");
  if (URL.empty() || !validUtf8(URL))
    return R;
  const auto FragmentAt = URL.find('#'), QueryAt = URL.find('?');
  R.FragmentPresent = FragmentAt != std::string_view::npos;
  // Count lexical query components even when a relative/unsupported URL
  // cannot produce a comparison key. Preview must not hide this omission.
  if (QueryAt != std::string_view::npos &&
      (FragmentAt == std::string_view::npos || QueryAt < FragmentAt)) {
    auto Query = URL.substr(QueryAt + 1, FragmentAt == std::string_view::npos
                                             ? std::string_view::npos
                                             : FragmentAt - QueryAt - 1);
    while (!Query.empty()) {
      const auto Amp = Query.find('&');
      if (Amp != 0)
        ++R.QueryParameters;
      if (Amp == std::string_view::npos)
        break;
      Query.remove_prefix(Amp + 1);
    }
  }
  // Do not silently trim embedded controls from evidence before comparison.
  for (const unsigned char C : URL)
    if (C <= 0x20 || C == 0x7f)
      return R;
  auto Parsed = ada::parse<ada::url_aggregator>(URL);
  if (!Parsed) {
    // A base is intentionally absent: relative references are not promoted
    // to absolute endpoints by a made-up origin.
    if (URL.front() == '/' || URL.front() == '.' ||
        URL.find(':') == std::string_view::npos)
      R.Status = "relative_unresolved";
    return R;
  }
  const auto Protocol = Parsed->get_protocol();
  if (Protocol != "http:" && Protocol != "https:" && Protocol != "ws:" &&
      Protocol != "wss:") {
    R.Status = "unsupported_scheme";
    return R;
  }
  R.Scheme = std::string(Protocol.substr(0, Protocol.size() - 1));
  // A scheme alone is not an absolute-reference proof. Special URLs such as
  // https:host/path or https:/host/path can resolve relative to the calling
  // document. This profile deliberately has no environment base.
  const auto Rest = URL.substr(URL.find(':') + 1);
  if (!Rest.starts_with("//") || Rest.size() < 3 || Rest[2] == '/' ||
      Rest[2] == '\\') {
    R.Status = "base_dependent_url";
    return R;
  }
  R.Status = Protocol == "http:" || Protocol == "https:" ? "absolute_http"
                                                         : "absolute_websocket";
  R.CredentialsPresent =
      !Parsed->get_username().empty() || !Parsed->get_password().empty();
  R.Origin = Parsed->get_origin();
  R.Path = Parsed->get_pathname();
  if (R.Origin.size() + R.Path.size() > MaxInterfaceURLBytes)
    throw Error("interface_url_budget_exceeded");
  // Nonempty segments only; no percent-decoding or route inference.
  bool Segment = false;
  for (const auto C : R.Path) {
    if (C == '/')
      Segment = false;
    else if (!Segment) {
      ++R.PathSegments;
      Segment = true;
    }
  }
  return R;
}
} // namespace neverd::web
