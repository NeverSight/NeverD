//===- ResolvedHostTarget.cpp - Deterministic host target identity -------===//

#include "neverd/translate/ResolvedHostTarget.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd::translate {
namespace {

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

bool isLowerASCIIAlnum(char Character) {
  const unsigned char Byte = static_cast<unsigned char>(Character);
  return (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9');
}

bool isCanonicalAtom(llvm::StringRef Text) {
  if (Text.empty() || !isLowerASCIIAlnum(Text.front()))
    return false;
  return llvm::all_of(Text, [](char Character) {
    return isLowerASCIIAlnum(Character) || Character == '_' ||
           Character == '.' || Character == '-';
  });
}

bool isCanonicalTripleComponent(llvm::StringRef Text) {
  if (Text.empty() || !isLowerASCIIAlnum(Text.front()))
    return false;
  return llvm::all_of(Text, [](char Character) {
    return isLowerASCIIAlnum(Character) || Character == '_' ||
           Character == '.' || Character == '+';
  });
}

bool isStrictTripleText(llvm::StringRef Text) {
  if (Text.empty() || Text.starts_with('-') || Text.ends_with('-'))
    return false;
  llvm::SmallVector<llvm::StringRef, 5> Components;
  Text.split(Components, '-', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  return Components.size() >= 3 && Components.size() <= 5 &&
         llvm::all_of(Components, isCanonicalTripleComponent);
}

std::optional<GuestArchitecture>
architectureFromTriple(const llvm::Triple &Triple) {
  switch (Triple.getArch()) {
  case llvm::Triple::x86:
    return GuestArchitecture::X86_32;
  case llvm::Triple::x86_64:
    return GuestArchitecture::X86_64;
  case llvm::Triple::arm:
  case llvm::Triple::thumb:
    return GuestArchitecture::ARM32;
  case llvm::Triple::aarch64:
    return GuestArchitecture::AArch64;
  default:
    return std::nullopt;
  }
}

llvm::StringLiteral architectureName(GuestArchitecture Architecture) {
  switch (Architecture) {
  case GuestArchitecture::X86_32:
    return "x86";
  case GuestArchitecture::X86_64:
    return "x86_64";
  case GuestArchitecture::ARM32:
    return "arm";
  case GuestArchitecture::AArch64:
    return "aarch64";
  }
  llvm_unreachable("unsupported resolved host architecture");
}

llvm::Expected<std::pair<std::string, GuestArchitecture>>
normalizeTriple(llvm::StringRef RequestedTriple) {
  if (RequestedTriple.empty())
    return invalid("host target triple must not be empty");
  if (!llvm::all_of(RequestedTriple, [](char Character) {
        const unsigned char Byte = static_cast<unsigned char>(Character);
        return (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9') ||
               Character == '_' || Character == '.' || Character == '+' ||
               Character == '-';
      }))
    return invalid("host target triple is not lower-case ASCII");

  std::string Normalized = llvm::Triple::normalize(RequestedTriple);
  llvm::Triple Parsed(Normalized);
  const std::optional<GuestArchitecture> Architecture =
      architectureFromTriple(Parsed);
  if (!Architecture)
    return invalid("host target triple has an unsupported architecture");

  // Collapse only spelling aliases that carry no subarchitecture semantics.
  // ISA-bearing spellings such as i686, armv7, thumbv7, and arm64e remain
  // distinct code-generation targets.
  if (Parsed.getArchName() == "amd64")
    Parsed.setArchName("x86_64");
  else if (Parsed.getArchName() == "arm64")
    Parsed.setArchName("aarch64");
  Normalized = Parsed.normalize();

  if (!isStrictTripleText(Normalized))
    return invalid("host target triple does not normalize to a complete "
                   "canonical triple");
  return std::make_pair(std::move(Normalized), *Architecture);
}

llvm::Expected<std::vector<std::string>>
normalizeFeatures(llvm::ArrayRef<std::string> RequestedFeatures) {
  std::set<std::string> FeatureNames;
  std::vector<std::string> Features;
  Features.reserve(RequestedFeatures.size());
  for (const std::string &Feature : RequestedFeatures) {
    if (Feature.size() <= 1 ||
        (Feature.front() != '+' && Feature.front() != '-') ||
        !isCanonicalAtom(llvm::StringRef(Feature).drop_front()))
      return invalid("host target feature must be a nonempty signed "
                     "lower-case ASCII name");
    if (!FeatureNames.insert(Feature.substr(1)).second)
      return invalid("host target contains duplicate or conflicting features");
    Features.push_back(Feature);
  }
  llvm::sort(Features);
  return Features;
}

std::string createCacheKey(HostTargetKind RequestedKind,
                           GuestArchitecture Architecture,
                           llvm::StringRef Triple, llvm::StringRef CPU,
                           llvm::ArrayRef<std::string> Features) {
  std::string Key;
  llvm::raw_string_ostream Stream(Key);
  Stream << "neverd.host-target.v" << ResolvedHostTarget::CacheIdentityVersion
         << '\n'
         << "requested="
         << (RequestedKind == HostTargetKind::Native ? "native" : "explicit")
         << "\narchitecture=" << architectureName(Architecture)
         << "\ntriple=" << Triple << "\ncpu=" << CPU
         << "\nfeatures=" << Features.size() << '\n';
  for (const std::string &Feature : Features)
    Stream << "feature=" << Feature << '\n';
  return Key;
}

llvm::Error validateModeAndRequest(const TranslationOptions &Options) {
  if (Options.Mode != TranslationMode::JIT &&
      Options.Mode != TranslationMode::AOT)
    return invalid("unknown translation mode");
  if (Options.Target.Kind != HostTargetKind::Native &&
      Options.Target.Kind != HostTargetKind::Explicit)
    return invalid("unknown host-target kind");

  if (Options.Mode == TranslationMode::JIT &&
      Options.Target.Kind != HostTargetKind::Native)
    return invalid("JIT translation only accepts the native process target");
  if (Options.Mode == TranslationMode::AOT &&
      Options.Target.Kind != HostTargetKind::Explicit)
    return invalid("AOT translation requires an explicit host target");
  return llvm::Error::success();
}

} // namespace

llvm::Expected<ResolvedHostTarget>
ResolvedHostTarget::resolve(const TranslationOptions &Options) {
  if (llvm::Error Error = validateModeAndRequest(Options))
    return std::move(Error);

  const HostTarget RequestedTarget = Options.Target;
  if (Options.Target.Kind == HostTargetKind::Native) {
    if (Options.Target.Architecture || !Options.Target.Triple.empty() ||
        !Options.Target.CPU.empty() || !Options.Target.Features.empty())
      return invalid("native host target cannot carry explicit target fields");
    if (llvm::Error Error = validateTranslationOptions(Options))
      return std::move(Error);

    llvm::Expected<std::pair<std::string, GuestArchitecture>> Triple =
        normalizeTriple(llvm::sys::getProcessTriple());
    if (!Triple)
      return Triple.takeError();

    const std::string CPU = llvm::sys::getHostCPUName().str();
    if (!isCanonicalAtom(CPU))
      return invalid("native host CPU could not be resolved to a canonical "
                     "name");

    std::vector<std::string> DetectedFeatures;
    for (const auto &[Name, Enabled] : llvm::sys::getHostCPUFeatures())
      DetectedFeatures.push_back((Enabled ? "+" : "-") + Name.str());
    llvm::Expected<std::vector<std::string>> Features =
        normalizeFeatures(DetectedFeatures);
    if (!Features)
      return Features.takeError();

    std::string CacheKey = createCacheKey(
        HostTargetKind::Native, Triple->second, Triple->first, CPU, *Features);
    return ResolvedHostTarget(RequestedTarget, Triple->second,
                              std::move(Triple->first), CPU,
                              std::move(*Features), std::move(CacheKey));
  }

  if (!Options.Target.Architecture)
    return invalid("explicit host target is missing its architecture");
  if (!getArchitectureDescription(*Options.Target.Architecture))
    return invalid("explicit host target has an unknown architecture");

  llvm::Expected<std::pair<std::string, GuestArchitecture>> Triple =
      normalizeTriple(Options.Target.Triple);
  if (!Triple)
    return Triple.takeError();
  if (Triple->second != *Options.Target.Architecture)
    return invalid("explicit host target triple does not match its "
                   "architecture");
  if (!Options.Target.CPU.empty() && !isCanonicalAtom(Options.Target.CPU))
    return invalid("explicit host CPU is not canonical lower-case ASCII");

  llvm::Expected<std::vector<std::string>> Features =
      normalizeFeatures(Options.Target.Features);
  if (!Features)
    return Features.takeError();

  // Validation consumes a canonical copy while requestedTarget() retains the
  // caller's spelling and feature order.  This keeps the established options
  // policy authoritative without making cache identity input-order dependent.
  TranslationOptions CanonicalOptions = Options;
  CanonicalOptions.Target.Triple = Triple->first;
  CanonicalOptions.Target.Features = *Features;
  if (llvm::Error Error = validateTranslationOptions(CanonicalOptions))
    return std::move(Error);

  std::string CacheKey =
      createCacheKey(HostTargetKind::Explicit, Triple->second, Triple->first,
                     Options.Target.CPU, *Features);
  return ResolvedHostTarget(RequestedTarget, Triple->second,
                            std::move(Triple->first), Options.Target.CPU,
                            std::move(*Features), std::move(CacheKey));
}

llvm::Expected<ResolvedHostTarget>
resolveHostTarget(const TranslationOptions &Options) {
  return ResolvedHostTarget::resolve(Options);
}

} // namespace neverd::translate
