//===- SafetyTypes.cpp - Enum spellings for the safety vocabulary ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/SafetyTypes.h"

#include "llvm/Support/ErrorHandling.h"

namespace neverd::safety {

const char *toString(Track T) {
  switch (T) {
#define SAFETY_TRACK(ID, SPELLING)                                             \
  case Track::ID:                                                              \
    return SPELLING;
#include "neverd/safety/SafetyEnums.def"
  }
  llvm_unreachable("unknown safety track");
}

const char *toString(Verdict V) {
  switch (V) {
#define SAFETY_VERDICT(ID, SPELLING)                                           \
  case Verdict::ID:                                                            \
    return SPELLING;
#include "neverd/safety/SafetyEnums.def"
  }
  llvm_unreachable("unknown safety verdict");
}

const char *toString(Confidence C) {
  switch (C) {
#define SAFETY_CONFIDENCE(ID, SPELLING)                                        \
  case Confidence::ID:                                                         \
    return SPELLING;
#include "neverd/safety/SafetyEnums.def"
  }
  llvm_unreachable("unknown safety confidence");
}

const char *toString(VulnClass C) {
  switch (C) {
#define SAFETY_VULN_CLASS(ID, SPELLING)                                        \
  case VulnClass::ID:                                                          \
    return SPELLING;
#include "neverd/safety/SafetyEnums.def"
  }
  llvm_unreachable("unknown safety vulnerability class");
}

const char *toString(SinkKind K) {
  switch (K) {
#define SAFETY_SINK_KIND(ID, SPELLING)                                         \
  case SinkKind::ID:                                                           \
    return SPELLING;
#include "neverd/safety/SafetyEnums.def"
  }
  llvm_unreachable("unknown safety sink kind");
}

const char *toString(NameSource S) {
  switch (S) {
#define SAFETY_NAME_SOURCE(ID, SPELLING)                                       \
  case NameSource::ID:                                                         \
    return SPELLING;
#include "neverd/safety/SafetyEnums.def"
  }
  llvm_unreachable("unknown safety name source");
}

const char *toString(ArgFlow F) {
  switch (F) {
#define SAFETY_ARG_FLOW(ID, SPELLING)                                          \
  case ArgFlow::ID:                                                            \
    return SPELLING;
#include "neverd/safety/SafetyEnums.def"
  }
  llvm_unreachable("unknown safety argument flow");
}

} // namespace neverd::safety
