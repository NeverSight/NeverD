#include "PathPolicy.h"

#include "Internal.h"

#ifdef NEVERD_WEB_ICU
#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/uversion.h>
#endif

namespace neverd::web {
bool archivePathsAvailable() {
#ifdef NEVERD_WEB_ICU
  UVersionInfo Version;
  u_getVersion(Version);
  UVersionInfo Unicode;
  u_getUnicodeVersion(Unicode);
  return Version[0] == 77 && Version[1] == 1 && Unicode[0] == 16 &&
         Unicode[1] == 0;
#else
  return false;
#endif
}

std::string archivePathKey(std::string_view Path) {
  if (!archivePathsAvailable())
    throw Error("archive_path_policy_unavailable");
  if (Path.empty() || Path.size() > 4096)
    throw Error("unsafe_member_name");
  std::string Key;
  while (!Path.empty()) {
    const auto Slash = Path.find('/');
    const auto Name = Path.substr(0, Slash);
    validateMemberName(Name);
#ifdef NEVERD_WEB_ICU
    UErrorCode Status = U_ZERO_ERROR;
    const auto *NFC = icu::Normalizer2::getNFCInstance(Status);
    if (U_FAILURE(Status))
      throw Error("archive_path_policy_unavailable");
    auto Text = icu::UnicodeString::fromUTF8(
        icu::StringPiece(Name.data(), int32_t(Name.size())));
    icu::UnicodeString Normalized;
    NFC->normalize(Text, Normalized, Status);
    Normalized.foldCase(U_FOLD_CASE_DEFAULT);
    Text.remove();
    NFC->normalize(Normalized, Text, Status);
    if (U_FAILURE(Status))
      throw Error("archive_path_policy_unavailable");
    std::string Component;
    Text.toUTF8String(Component);
    // Windows also reserves COM/LPT with superscript 1, 2 or 3. Do not
    // compatibility-normalize the actual evidence path to implement this.
    const auto Stem =
        std::string_view(Component).substr(0, Component.find('.'));
    if ((Stem.starts_with("com") || Stem.starts_with("lpt")) &&
        (Stem.substr(3) == "¹" || Stem.substr(3) == "²" ||
         Stem.substr(3) == "³"))
      throw Error("unsafe_member_name");
    if (!Key.empty())
      Key += '/';
    Key += Component;
#endif
    if (Slash == std::string_view::npos)
      break;
    Path.remove_prefix(Slash + 1);
    if (Path.empty())
      throw Error("unsafe_member_name");
  }
  return Key;
}
} // namespace neverd::web
