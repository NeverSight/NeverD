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

const char *toString(CapacityPrecision P) {
  switch (P) {
  case CapacityPrecision::Unknown:
    return "unknown";
  case CapacityPrecision::ContainerUpperBound:
    return "container_upper_bound";
  case CapacityPrecision::StorageExact:
    return "storage_exact";
  case CapacityPrecision::TypedBufferExact:
    return "typed_buffer_exact";
  }
  llvm_unreachable("unknown capacity precision");
}

const char *toString(ReplayInputKind K) {
  switch (K) {
#define SAFETY_REPLAY_INPUT_KIND(ID, SPELLING)                                 \
  case ReplayInputKind::ID:                                                    \
    return SPELLING;
#include "neverd/safety/SafetyEnums.def"
  }
  llvm_unreachable("unknown replay input kind");
}

const char *toString(ReplayBindingRole R) {
  switch (R) {
#define SAFETY_REPLAY_BINDING_ROLE(ID, SPELLING)                               \
  case ReplayBindingRole::ID:                                                  \
    return SPELLING;
#include "neverd/safety/SafetyEnums.def"
  }
  llvm_unreachable("unknown replay binding role");
}

const char *toString(ReachabilityStatus S) {
  switch (S) {
#define SAFETY_REACHABILITY_STATUS(ID, SPELLING)                               \
  case ReachabilityStatus::ID:                                                 \
    return SPELLING;
#include "neverd/safety/SafetyEnums.def"
  }
  llvm_unreachable("unknown safety reachability status");
}

const char *toString(SafetyEntryKind K) {
  switch (K) {
#define SAFETY_ENTRY_KIND(ID, SPELLING)                                        \
  case SafetyEntryKind::ID:                                                    \
    return SPELLING;
#include "neverd/safety/SafetyEnums.def"
  }
  llvm_unreachable("unknown safety entry kind");
}

} // namespace neverd::safety
