//===- NeverDCAPISanitize.cpp - Transactional native sanitizer C API ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/codegen/COFF/PESignaturePolicy.h"
#include "neverd/backend/codegen/MachO/MachOSignaturePolicy.h"
#include "neverd/backend/codegen/MachO/MachOSigner.h"
#include "neverd/backend/llvm/SafetyCallsiteMetadata.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/pass/ir/safety/RuntimeBoundsSanitizer.h"
#include "neverd/safety/DarwinSanitizerPublicationAdapter.h"
#include "neverd/safety/RuntimeSanitizer.h"
#include "neverd/safety/Safety.h"
#include "neverd/support/AtomicOutput.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

static_assert(std::is_nothrow_move_assignable_v<PatchResult>,
              "publication commit requires noexcept PatchResult move");

namespace {

#define FIELD_END(Type, Field)                                                 \
  (offsetof(Type, Field) + sizeof(static_cast<Type *>(nullptr)->Field))

constexpr bool reaches(size_t Size, size_t End) noexcept { return Size >= End; }

/// Minimal, allocation-free truth retained across the C ABI's exception
/// boundary.  In particular, an exception while formatting a diagnostic must
/// never turn an invoked or completed namespace publication back into
/// NOT_ATTEMPTED.
enum class PublicationExceptionState : uint8_t {
  NotInvoked,
  NotPublished,
  Indeterminate,
  Published,
};

#define SPLITS_FIELD(Type, Field, Size)                                        \
  ((Size) > offsetof(Type, Field) && (Size) < FIELD_END(Type, Field))

bool splitsKnownOptionsField(size_t Size) {
  return SPLITS_FIELD(neverd_sanitize_options_v1, strategy, Size) ||
         SPLITS_FIELD(neverd_sanitize_options_v1, max_paths, Size) ||
         SPLITS_FIELD(neverd_sanitize_options_v1, max_steps, Size) ||
         SPLITS_FIELD(neverd_sanitize_options_v1, max_loop, Size) ||
         SPLITS_FIELD(neverd_sanitize_options_v1, solver_conflicts, Size) ||
         SPLITS_FIELD(neverd_sanitize_options_v1, max_call_depth, Size) ||
         SPLITS_FIELD(neverd_sanitize_options_v1, max_summary_iterations, Size);
}

bool splitsKnownResultField(size_t Size) {
  return SPLITS_FIELD(neverd_sanitize_result_v1, ok, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, status, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, plan_version, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, findings, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, guarded_sites, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, guarded_functions, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, unsupported_sites, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, patched_functions, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, code_size, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, trampoline_count, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, publication_outcome, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, publication_receipt_version,
                      Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, publication_receipt_complete,
                      Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1,
                      publication_namespace_disposition, Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, publication_guarantee_flags,
                      Size) ||
         SPLITS_FIELD(neverd_sanitize_result_v1, publication_operand_binding,
                      Size);
}

template <typename ValueT>
void writeResult(neverd_sanitize_result_v1 *Result, size_t End,
                 ValueT neverd_sanitize_result_v1::*Field,
                 ValueT Value) noexcept {
  if (Result && reaches(Result->struct_size, End))
    Result->*Field = Value;
}

#define WRITE_RESULT(Result, Field, Value)                                     \
  writeResult((Result), FIELD_END(neverd_sanitize_result_v1, Field),           \
              &neverd_sanitize_result_v1::Field, (Value))

bool readBytes(const std::filesystem::path &Path, std::vector<uint8_t> &Bytes,
               std::string &Error) {
  auto Buffer = llvm::MemoryBuffer::getFile(Path.string(), false);
  if (!Buffer) {
    Error =
        "cannot read '" + Path.string() + "': " + Buffer.getError().message();
    return false;
  }
  const llvm::StringRef Contents = (*Buffer)->getBuffer();
  const auto *Begin = reinterpret_cast<const uint8_t *>(Contents.data());
  Bytes.assign(Begin, Begin + Contents.size());
  return true;
}

bool captureSafePermissions(const std::filesystem::path &Source,
                            std::filesystem::perms &Permissions,
                            std::string &Error) {
  std::error_code EC;
  Permissions = std::filesystem::status(Source, EC).permissions();
  if (EC) {
    Error = "cannot authenticate source permissions: " + EC.message();
    return false;
  }
  Permissions &=
      ~(std::filesystem::perms::set_uid | std::filesystem::perms::set_gid);
  return true;
}

bool requireBytes(const std::filesystem::path &Path,
                  llvm::ArrayRef<uint8_t> Expected, std::string &Error,
                  llvm::StringRef Stage) {
  std::vector<uint8_t> Actual;
  if (!readBytes(Path, Actual, Error)) {
    Error = Stage.str() + ": " + Error;
    return false;
  }
  if (!std::equal(Actual.begin(), Actual.end(), Expected.begin(),
                  Expected.end())) {
    Error = Stage.str() + ": candidate bytes changed during transaction";
    return false;
  }
  return true;
}

bool requirePermissions(const std::filesystem::path &Path,
                        std::filesystem::perms Expected, std::string &Error,
                        llvm::StringRef Stage) {
  std::error_code EC;
  const std::filesystem::perms Actual =
      std::filesystem::status(Path, EC).permissions();
  if (EC) {
    Error =
        Stage.str() + ": cannot inspect candidate permissions: " + EC.message();
    return false;
  }
  if ((Actual & std::filesystem::perms::mask) !=
      (Expected & std::filesystem::perms::mask)) {
    Error = Stage.str() + ": candidate permissions changed during transaction";
    return false;
  }
  return true;
}

class ScopedTemporaryFile {
public:
  ScopedTemporaryFile() = default;
  ScopedTemporaryFile(const ScopedTemporaryFile &) = delete;
  ScopedTemporaryFile &operator=(const ScopedTemporaryFile &) = delete;
  ~ScopedTemporaryFile() {
    if (File)
      llvm::consumeError(support::atomic_output::discardTemporaryOutput(*File));
  }

  bool create(const std::filesystem::path &OutputPath, llvm::StringRef Role,
              std::string &Error) {
    std::filesystem::path Directory = OutputPath.parent_path();
    if (Directory.empty())
      Directory = ".";
    llvm::SmallString<256> Model(Directory.string());
    const std::string Name = "." + OutputPath.filename().string() +
                             ".neverd-sanitize-%%%%%%." + Role.str();
    llvm::sys::path::append(Model, Name);
    llvm::Expected<llvm::sys::fs::TempFile> Created =
        llvm::sys::fs::TempFile::create(Model);
    if (!Created) {
      Error = "cannot create sanitizer " + Role.str() +
              " temp: " + llvm::toString(Created.takeError());
      return false;
    }
    File.emplace(std::move(*Created));
    Path = File->TmpName;
    return true;
  }

  bool write(llvm::ArrayRef<uint8_t> Bytes, std::string &Error) const {
    if (!File || File->FD == -1) {
      Error = "sanitizer temp is not open";
      return false;
    }
    llvm::raw_fd_ostream Stream(File->FD, false);
    if (!Bytes.empty())
      Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Stream.flush();
    if (Stream.has_error()) {
      Error = "cannot write sanitizer temp '" + Path.string() + "'";
      return false;
    }
    return true;
  }

  bool setPermissions(std::filesystem::perms Permissions,
                      std::string &Error) const {
    std::error_code EC;
    std::filesystem::permissions(Path, Permissions,
                                 std::filesystem::perm_options::replace, EC);
    if (EC) {
      Error = "cannot set sanitizer temp permissions: " + EC.message();
      return false;
    }
    return true;
  }

  const std::filesystem::path &path() const { return Path; }

private:
  std::optional<llvm::sys::fs::TempFile> File;
  std::filesystem::path Path;
};

/// Backend patchers publish to their output path internally, so a TempFile
/// handle opened before codegen would no longer identify the resulting inode.
/// Keep this intermediate candidate continuously registered for signal
/// cleanup, but closed.  Only its authenticated bytes are passed to the native
/// publication adapter; this pathname is never the namespace-publish operand.
class ScopedPathTemporaryFile {
public:
  ScopedPathTemporaryFile() = default;
  ScopedPathTemporaryFile(const ScopedPathTemporaryFile &) = delete;
  ScopedPathTemporaryFile &operator=(const ScopedPathTemporaryFile &) = delete;
  ~ScopedPathTemporaryFile() {
    std::string Ignored;
    removeBeforeCommit(Ignored);
  }

  bool create(const std::filesystem::path &OutputPath, llvm::StringRef Role,
              std::string &Error) {
    std::filesystem::path Directory = OutputPath.parent_path();
    if (Directory.empty())
      Directory = ".";
    llvm::SmallString<256> Model(Directory.string());
    const std::string Name = "." + OutputPath.filename().string() +
                             ".neverd-sanitize-%%%%%%." + Role.str();
    llvm::sys::path::append(Model, Name);
    llvm::SmallString<256> Created;
    int FD = -1;
    if (std::error_code EC =
            llvm::sys::fs::createUniqueFile(Model, FD, Created)) {
      Error =
          "cannot create sanitizer " + Role.str() + " temp: " + EC.message();
      return false;
    }
    Path = Created.c_str();
    std::string RegistrationDetail;
    if (llvm::sys::RemoveFileOnSignal(Path.string(), &RegistrationDetail)) {
      llvm::sys::Process::SafelyCloseFileDescriptor(FD);
      std::error_code EC = llvm::sys::fs::remove(Path.string());
      (void)EC;
      Path.clear();
      Error = "cannot register sanitizer " + Role.str() +
              " temp for signal cleanup";
      if (!RegistrationDetail.empty())
        Error += ": " + RegistrationDetail;
      return false;
    }
    if (std::error_code EC =
            llvm::sys::Process::SafelyCloseFileDescriptor(FD)) {
      llvm::sys::DontRemoveFileOnSignal(Path.string());
      std::error_code RemoveError = llvm::sys::fs::remove(Path.string());
      (void)RemoveError;
      Path.clear();
      Error = "cannot close sanitizer " + Role.str() + " temp: " + EC.message();
      return false;
    }
    return true;
  }

  bool setPermissions(std::filesystem::perms Permissions,
                      std::string &Error) const {
    std::error_code EC;
    std::filesystem::permissions(Path, Permissions,
                                 std::filesystem::perm_options::replace, EC);
    if (EC) {
      Error = "cannot set sanitizer temp permissions: " + EC.message();
      return false;
    }
    return true;
  }

  bool write(llvm::ArrayRef<uint8_t> Bytes, std::string &Error) const {
    std::error_code EC;
    llvm::raw_fd_ostream Stream(Path.string(), EC, llvm::sys::fs::OF_None);
    if (EC) {
      Error = "cannot open sanitizer path temp for writing: " + EC.message();
      return false;
    }
    if (!Bytes.empty())
      Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Stream.close();
    if (Stream.has_error()) {
      Error = "cannot write sanitizer path temp '" + Path.string() + "'";
      return false;
    }
    return true;
  }

  const std::filesystem::path &path() const { return Path; }

  bool removeBeforeCommit(std::string &Error) {
    if (Path.empty())
      return true;
    const std::filesystem::path RemovedPath = std::move(Path);
    Path.clear();
    std::error_code EC = llvm::sys::fs::remove(RemovedPath.string());
    llvm::sys::DontRemoveFileOnSignal(RemovedPath.string());
    if (EC && EC != llvm::errc::no_such_file_or_directory) {
      Error = "cannot remove sanitizer backend candidate before publication: " +
              EC.message();
      return false;
    }
    return true;
  }

private:
  std::filesystem::path Path;
};

safety::SafetyBudgets readBudgets(const neverd_sanitize_options_v1 *Options) {
  safety::SafetyBudgets Budgets;
  const size_t Size = Options->struct_size;
  if (reaches(Size, FIELD_END(neverd_sanitize_options_v1, max_paths)) &&
      Options->max_paths)
    Budgets.MaxPaths = Options->max_paths;
  if (reaches(Size, FIELD_END(neverd_sanitize_options_v1, max_steps)) &&
      Options->max_steps)
    Budgets.MaxSteps = Options->max_steps;
  if (reaches(Size, FIELD_END(neverd_sanitize_options_v1, max_loop)) &&
      Options->max_loop)
    Budgets.MaxLoop = Options->max_loop;
  if (reaches(Size, FIELD_END(neverd_sanitize_options_v1, solver_conflicts)) &&
      Options->solver_conflicts)
    Budgets.SolverConflicts = Options->solver_conflicts;
  if (reaches(Size, FIELD_END(neverd_sanitize_options_v1, max_call_depth)) &&
      Options->max_call_depth)
    Budgets.MaxCallDepth = Options->max_call_depth;
  if (reaches(Size,
              FIELD_END(neverd_sanitize_options_v1, max_summary_iterations)) &&
      Options->max_summary_iterations)
    Budgets.MaxSummaryIterations = Options->max_summary_iterations;
  return Budgets;
}

bool isNativeTarget(const BinaryImage &Image) {
  const bool Format = Image.Format == BinaryFormat::ELF ||
                      Image.Format == BinaryFormat::COFF ||
                      Image.Format == BinaryFormat::MachO;
  return Format && archLiftSupported(Image.Arch);
}

enum class NativeImageKind : uint8_t {
  MainExecutable,
  Relocatable,
  DynamicLibrary,
  UniversalMachO,
};

llvm::Error targetContainerError(llvm::StringRef Detail) {
  return llvm::make_error<llvm::StringError>(
      "sanitize target container: " + Detail, llvm::inconvertibleErrorCode());
}

template <typename ELFT>
llvm::Expected<bool>
hasELFInterpreter(const llvm::object::ELFObjectFile<ELFT> &Object) {
  auto Headers = Object.getELFFile().program_headers();
  if (!Headers)
    return Headers.takeError();
  return std::any_of(Headers->begin(), Headers->end(), [](const auto &Header) {
    return Header.p_type == llvm::ELF::PT_INTERP;
  });
}

llvm::Expected<bool>
hasELFInterpreter(const llvm::object::ELFObjectFileBase &Object) {
  if (const auto *Typed =
          llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(&Object))
    return hasELFInterpreter(*Typed);
  if (const auto *Typed =
          llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(&Object))
    return hasELFInterpreter(*Typed);
  if (const auto *Typed =
          llvm::dyn_cast<llvm::object::ELF32BEObjectFile>(&Object))
    return hasELFInterpreter(*Typed);
  if (const auto *Typed =
          llvm::dyn_cast<llvm::object::ELF64BEObjectFile>(&Object))
    return hasELFInterpreter(*Typed);
  return targetContainerError("ELF byte order or word size is unsupported");
}

llvm::Expected<NativeImageKind>
classifyNativeImage(llvm::ArrayRef<uint8_t> Bytes) {
  const llvm::StringRef Contents(reinterpret_cast<const char *>(Bytes.data()),
                                 Bytes.size());
  llvm::Expected<std::unique_ptr<llvm::object::Binary>> Parsed =
      llvm::object::createBinary(
          llvm::MemoryBufferRef(Contents, "<sanitize-source>"));
  if (!Parsed)
    return Parsed.takeError();

  if (llvm::isa<llvm::object::MachOUniversalBinary>(Parsed->get()))
    return NativeImageKind::UniversalMachO;
  if (const auto *MachO =
          llvm::dyn_cast<llvm::object::MachOObjectFile>(Parsed->get())) {
    switch (MachO->getHeader().filetype) {
    case llvm::MachO::MH_EXECUTE:
      return NativeImageKind::MainExecutable;
    case llvm::MachO::MH_OBJECT:
      return NativeImageKind::Relocatable;
    default:
      return NativeImageKind::DynamicLibrary;
    }
  }
  if (const auto *COFF =
          llvm::dyn_cast<llvm::object::COFFObjectFile>(Parsed->get())) {
    if (!COFF->getPE32Header() && !COFF->getPE32PlusHeader())
      return NativeImageKind::Relocatable;
    const llvm::object::coff_file_header *Header = COFF->getCOFFHeader();
    if (!Header)
      return targetContainerError("PE image has no COFF file header");
    if ((Header->Characteristics & llvm::COFF::IMAGE_FILE_DLL) != 0)
      return NativeImageKind::DynamicLibrary;
    return NativeImageKind::MainExecutable;
  }
  if (const auto *ELF =
          llvm::dyn_cast<llvm::object::ELFObjectFileBase>(Parsed->get())) {
    switch (ELF->getEType()) {
    case llvm::ELF::ET_REL:
      return NativeImageKind::Relocatable;
    case llvm::ELF::ET_EXEC:
      return NativeImageKind::MainExecutable;
    case llvm::ELF::ET_DYN: {
      llvm::Expected<bool> HasInterpreter = hasELFInterpreter(*ELF);
      if (!HasInterpreter)
        return HasInterpreter.takeError();
      return *HasInterpreter ? NativeImageKind::MainExecutable
                             : NativeImageKind::DynamicLibrary;
    }
    default:
      return targetContainerError("ELF image type is unsupported");
    }
  }
  return targetContainerError("input is not a native executable container");
}

struct GuardedSecurityContext {
  std::optional<macho_signature::Profile> MachO;
  bool RequireUnsignedPE = false;
};

bool isUnsignedMachO(const macho_signature::Profile &Profile) {
  return !Profile.Universal && Profile.Slices.size() == 1 &&
         Profile.Slices.front().SignatureKind ==
             macho_signature::Kind::Unsigned;
}

bool hasBundleExtension(std::filesystem::path Current) {
  while (!Current.empty()) {
    std::string Extension = Current.extension().string();
    std::transform(Extension.begin(), Extension.end(), Extension.begin(),
                   [](char Value) {
                     return static_cast<char>(
                         std::tolower(static_cast<unsigned char>(Value)));
                   });
    if (Extension == ".app" || Extension == ".framework" ||
        Extension == ".xpc" || Extension == ".appex" ||
        Extension == ".bundle" || Extension == ".plugin" ||
        Extension == ".kext")
      return true;
    const std::filesystem::path Parent = Current.parent_path();
    if (Parent == Current)
      break;
    Current = Parent;
  }
  return false;
}

bool isBundleMember(const std::filesystem::path &Member, bool &IsMember,
                    std::string &Error) {
  IsMember = false;
  std::error_code EC;
  const std::filesystem::path Absolute = std::filesystem::absolute(Member, EC);
  if (EC) {
    Error = "cannot resolve bundle-member path: " + EC.message();
    return false;
  }
  const std::filesystem::path LexicalParent =
      Absolute.lexically_normal().parent_path();
  if (hasBundleExtension(LexicalParent)) {
    IsMember = true;
    return true;
  }
  const std::filesystem::path ResolvedParent =
      std::filesystem::weakly_canonical(Absolute.parent_path(), EC);
  if (EC) {
    Error = "cannot canonicalize bundle-member ancestors: " + EC.message();
    return false;
  }
  IsMember = hasBundleExtension(ResolvedParent);
  return true;
}

bool normalizePublicationDestination(const std::filesystem::path &Requested,
                                     std::filesystem::path &Normalized,
                                     bool &IsBundleMember, std::string &Error) {
  IsBundleMember = false;
  std::error_code EC;
  std::filesystem::path Absolute = std::filesystem::absolute(Requested, EC);
  if (EC) {
    Error = "cannot make sanitizer output path absolute: " + EC.message();
    return false;
  }
  Absolute = Absolute.lexically_normal();
  if (Absolute.filename().empty()) {
    Error = "sanitize output path must name a file";
    return false;
  }
  // Derive the lexical policy decision and final path from this one absolute
  // capture.  In particular, never resolve a relative destination once for a
  // bundle check and a second time for publication.
  const bool LexicalBundleMember = hasBundleExtension(Absolute.parent_path());
  const std::filesystem::path Parent =
      std::filesystem::canonical(Absolute.parent_path(), EC);
  if (EC) {
    // A lexically named bundle is rejected by the caller without publishing,
    // even when its not-yet-created parent cannot be canonicalized.
    if (LexicalBundleMember) {
      Normalized = Absolute;
      IsBundleMember = true;
      return true;
    }
    Error = "cannot canonicalize sanitizer output parent: " + EC.message();
    return false;
  }
  Normalized = Parent / Absolute.filename();
  IsBundleMember = LexicalBundleMember || hasBundleExtension(Parent);
  return true;
}

bool prepareGuardedSecurity(Session *S, const std::filesystem::path &SourcePath,
                            llvm::ArrayRef<uint8_t> CurrentSource,
                            const std::filesystem::path &OutputPath,
                            std::filesystem::perms SafePermissions,
                            ScopedPathTemporaryFile &VerificationSnapshot,
                            GuardedSecurityContext &Context,
                            neverd_sanitize_status_t &FailureStatus,
                            std::string &Error) {
  Error.clear();
  FailureStatus = NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED;
  if (S->Img.Format == BinaryFormat::MachO) {
    bool SourceIsBundleMember = false;
    if (!isBundleMember(SourcePath, SourceIsBundleMember, Error))
      return false;
    if (SourceIsBundleMember) {
      FailureStatus = NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED;
      Error = "guarded Mach-O bundle member publication is unsupported";
      return false;
    }

    llvm::Expected<macho_signature::Profile> Profile =
        macho_signature::inspect(CurrentSource);
    if (!Profile) {
      Error = llvm::toString(Profile.takeError());
      return false;
    }
    if (Profile->Universal || Profile->Slices.size() != 1) {
      Error = "thin Mach-O signature profile is required";
      return false;
    }
    if (!isUnsignedMachO(*Profile)) {
      if (!macho_signature::canTransactionallyAdHocResign(*Profile)) {
        FailureStatus = NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED;
        Error = "Mach-O signature is not eligible for transactional ad-hoc "
                "re-signing";
        return false;
      }
#ifdef __APPLE__
      if (!VerificationSnapshot.create(OutputPath, "signature-source", Error) ||
          !VerificationSnapshot.write(CurrentSource, Error) ||
          !VerificationSnapshot.setPermissions(SafePermissions, Error) ||
          !requireBytes(VerificationSnapshot.path(), CurrentSource, Error,
                        "before source signature verification"))
        return false;
      if (llvm::Error Verify = macho_signing::verifyStrict(
              VerificationSnapshot.path().string())) {
        Error = "source signature verification failed: " +
                llvm::toString(std::move(Verify));
        return false;
      }
      if (!requireBytes(VerificationSnapshot.path(), CurrentSource, Error,
                        "after source signature verification"))
        return false;
#else
      FailureStatus = NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED;
      Error = "signed Mach-O publication requires the macOS codesign tool";
      return false;
#endif
    }
    Context.MachO = std::move(*Profile);
    return true;
  }

  if (S->Img.Format == BinaryFormat::COFF) {
    llvm::Expected<pe_signature::Profile> Profile =
        pe_signature::inspect(CurrentSource);
    if (!Profile) {
      Error = llvm::toString(Profile.takeError());
      return false;
    }
    if (Profile->SignatureKind != pe_signature::Kind::Unsigned) {
      FailureStatus = NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED;
      Error = "guarded Authenticode-signed PE publication is unsupported";
      return false;
    }
    Context.RequireUnsignedPE = true;
  }
  return true;
}

llvm::Error validateGuardedSecurityBytes(const GuardedSecurityContext &Context,
                                         llvm::ArrayRef<uint8_t> Bytes) {
  if (Context.MachO) {
    llvm::Expected<macho_signature::Profile> After =
        macho_signature::inspect(Bytes);
    if (!After)
      return After.takeError();
    if (isUnsignedMachO(*Context.MachO)) {
      if (!isUnsignedMachO(*After))
        return targetContainerError(
            "unsigned Mach-O candidate unexpectedly gained a signature");
    } else if (llvm::Error Validation =
                   macho_signature::validateTransactionallyAdHocResigned(
                       *Context.MachO, *After)) {
      return Validation;
    }
  }
  if (Context.RequireUnsignedPE) {
    llvm::Expected<pe_signature::Profile> After = pe_signature::inspect(Bytes);
    if (!After)
      return After.takeError();
    if (After->SignatureKind != pe_signature::Kind::Unsigned)
      return targetContainerError(
          "unsigned PE candidate unexpectedly gained a certificate table");
  }
  return llvm::Error::success();
}

bool authenticateReload(const BinaryImage &Original,
                        const BinaryImage &Reloaded, std::string &Error) {
  if (Original.Format != Reloaded.Format || Original.Arch != Reloaded.Arch ||
      Original.Mode != Reloaded.Mode || Original.Bits != Reloaded.Bits ||
      Original.IsRelocatable != Reloaded.IsRelocatable ||
      Original.Base != Reloaded.Base || Original.Entry != Reloaded.Entry) {
    Error = "published candidate changed binary format, architecture, mode, "
            "bitness, relocatable identity, image base, or entry point";
    return false;
  }
  return true;
}

bool collectCallsiteRecords(
    llvm::Module &Module,
    std::vector<safety_callsite_md::SafetyCallsiteRecord> &Records,
    std::string &Error) {
  for (llvm::Function &Function : Module)
    for (llvm::Instruction &Instruction : llvm::instructions(Function)) {
      auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
      if (!Call)
        continue;
      auto Parsed = safety_callsite_md::parse(*Call);
      if (!Parsed) {
        Error = llvm::toString(Parsed.takeError());
        return false;
      }
      if (*Parsed)
        Records.push_back(**Parsed);
    }
  return true;
}

size_t
uniqueGuardedFunctions(llvm::ArrayRef<safety::RuntimeSanitizerGuard> Guards) {
  std::vector<va_t> Entries;
  Entries.reserve(Guards.size());
  for (const safety::RuntimeSanitizerGuard &Guard : Guards)
    Entries.push_back(Guard.Occurrence.FuncEntry);
  std::sort(Entries.begin(), Entries.end());
  return static_cast<size_t>(std::unique(Entries.begin(), Entries.end()) -
                             Entries.begin());
}

void clearResult(neverd_sanitize_result_v1 *Result) noexcept {
  if (!Result)
    return;
  const size_t Size = Result->struct_size;
#define CLEAR_FIELD(Field)                                                     \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_sanitize_result_v1, Field)))            \
      Result->Field = {};                                                      \
  } while (false)
  CLEAR_FIELD(ok);
  CLEAR_FIELD(status);
  CLEAR_FIELD(plan_version);
  CLEAR_FIELD(findings);
  CLEAR_FIELD(guarded_sites);
  CLEAR_FIELD(guarded_functions);
  CLEAR_FIELD(unsupported_sites);
  CLEAR_FIELD(patched_functions);
  CLEAR_FIELD(code_size);
  CLEAR_FIELD(trampoline_count);
  CLEAR_FIELD(publication_outcome);
  CLEAR_FIELD(publication_receipt_version);
  CLEAR_FIELD(publication_receipt_complete);
  CLEAR_FIELD(publication_namespace_disposition);
  CLEAR_FIELD(publication_guarantee_flags);
  CLEAR_FIELD(publication_operand_binding);
#undef CLEAR_FIELD
}

void initializePublicationResult(neverd_sanitize_result_v1 *Result) noexcept {
  WRITE_RESULT(Result, publication_outcome,
               neverd_sanitize_publication_outcome_t{
                   NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED});
  WRITE_RESULT(Result, publication_receipt_version,
               uint32_t{NEVERD_SANITIZE_PUBLICATION_ABI_VERSION});
}

int fail(Session *S, neverd_sanitize_result_v1 *Result,
         neverd_sanitize_status_t Status, llvm::StringRef Message) {
  if (S)
    S->setError(Message.str());
  if (Result && reaches(Result->struct_size,
                        FIELD_END(neverd_sanitize_result_v1, status)))
    Result->status = Status;
  return 0;
}

int failUnexpected(neverd_session_t Sess, neverd_sanitize_result_v1 *Result,
                   llvm::StringRef Message,
                   PublicationExceptionState PublicationState) noexcept {
  clearResult(Result);
  initializePublicationResult(Result);

  neverd_sanitize_status_t Status = NEVERD_SANITIZE_STATUS_PIPELINE_FAILED;
  neverd_sanitize_publication_outcome_t Outcome =
      NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_ATTEMPTED;
  neverd_sanitize_publication_namespace_t Namespace =
      NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NONE;
  switch (PublicationState) {
  case PublicationExceptionState::NotInvoked:
    break;
  case PublicationExceptionState::NotPublished:
    Status = NEVERD_SANITIZE_STATUS_PUBLISH_FAILED;
    Outcome = NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_PUBLISHED;
    Namespace = NEVERD_SANITIZE_PUBLICATION_NAMESPACE_CREATE_EXCLUSIVE;
    break;
  case PublicationExceptionState::Indeterminate:
    Status = NEVERD_SANITIZE_STATUS_PUBLISH_INDETERMINATE;
    Outcome = NEVERD_SANITIZE_PUBLICATION_OUTCOME_INDETERMINATE;
    Namespace = NEVERD_SANITIZE_PUBLICATION_NAMESPACE_CREATE_EXCLUSIVE;
    break;
  case PublicationExceptionState::Published:
    Status = NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE;
    Outcome = NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED;
    Namespace = NEVERD_SANITIZE_PUBLICATION_NAMESPACE_CREATE_EXCLUSIVE;
    break;
  }
  WRITE_RESULT(Result, status, Status);
  WRITE_RESULT(Result, publication_outcome, Outcome);
  WRITE_RESULT(Result, publication_namespace_disposition, Namespace);

  // Diagnostics are secondary to the allocation-free terminal tuple above.
  // If storing the diagnostic itself fails, the caller still receives the
  // conservative publication state and can apply the required advisory.
  Session *S = toSession(Sess);
  if (S) {
    try {
      S->clearError();
      S->setError(Message.str());
    } catch (...) {
    }
  }
  return 0;
}

namespace publication = safety::sanitizer_publication_metadata;

neverd_sanitize_publication_outcome_t
publicOutcome(publication::SanitizerPublicationOutcome Outcome) {
  switch (Outcome) {
  case publication::SanitizerPublicationOutcome::NotPublished:
    return NEVERD_SANITIZE_PUBLICATION_OUTCOME_NOT_PUBLISHED;
  case publication::SanitizerPublicationOutcome::Published:
    return NEVERD_SANITIZE_PUBLICATION_OUTCOME_PUBLISHED;
  case publication::SanitizerPublicationOutcome::Indeterminate:
    return NEVERD_SANITIZE_PUBLICATION_OUTCOME_INDETERMINATE;
  }
  return NEVERD_SANITIZE_PUBLICATION_OUTCOME_INDETERMINATE;
}

neverd_sanitize_publication_namespace_t
publicNamespace(publication::PublicationNamespaceDisposition Disposition) {
  switch (Disposition) {
  case publication::PublicationNamespaceDisposition::None:
    return NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NONE;
  case publication::PublicationNamespaceDisposition::CreateExclusive:
    return NEVERD_SANITIZE_PUBLICATION_NAMESPACE_CREATE_EXCLUSIVE;
  case publication::PublicationNamespaceDisposition::NoChange:
    return NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NO_CHANGE;
  }
  return NEVERD_SANITIZE_PUBLICATION_NAMESPACE_NONE;
}

neverd_sanitize_publication_operand_binding_t
publicOperandBinding(publication::CandidatePublishOperandBindingV1 Binding) {
  switch (Binding) {
  case publication::CandidatePublishOperandBindingV1::None:
    return NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_NONE;
  case publication::CandidatePublishOperandBindingV1::
      AccessControlConfinedDistinctCredentials:
    return NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_ACCESS_CONTROL_CONFINED_DISTINCT_CREDENTIALS;
  case publication::CandidatePublishOperandBindingV1::KernelHeldObject:
    return NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_KERNEL_HELD_OBJECT;
  }
  return NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_NONE;
}

void writePublicationExecutionResult(
    neverd_sanitize_result_v1 *Result,
    const publication::SanitizerPublicationExecutionResultV1 &Execution) {
  WRITE_RESULT(Result, publication_outcome, publicOutcome(Execution.Outcome));
  WRITE_RESULT(Result, publication_receipt_version,
               uint32_t{Execution.Receipt.Version});
  WRITE_RESULT(Result, publication_receipt_complete,
               uint32_t{Execution.Receipt.Complete ? 1u : 0u});
  WRITE_RESULT(Result, publication_namespace_disposition,
               publicNamespace(Execution.Receipt.NamespaceDisposition));
  uint32_t Guarantees = 0;
  if (Execution.Receipt.Guarantees.NamespaceAtomic)
    Guarantees |= NEVERD_SANITIZE_PUBLICATION_GUARANTEE_NAMESPACE_ATOMIC;
  if (Execution.Receipt.Guarantees.DestinationCreateExclusive)
    Guarantees |=
        NEVERD_SANITIZE_PUBLICATION_GUARANTEE_DESTINATION_CREATE_EXCLUSIVE;
  if (Execution.Receipt.Guarantees.CompareAndSwap)
    Guarantees |= NEVERD_SANITIZE_PUBLICATION_GUARANTEE_COMPARE_AND_SWAP;
  if (Execution.Receipt.Guarantees.CrashDurable)
    Guarantees |= NEVERD_SANITIZE_PUBLICATION_GUARANTEE_CRASH_DURABLE;
  WRITE_RESULT(Result, publication_guarantee_flags, Guarantees);
  WRITE_RESULT(
      Result, publication_operand_binding,
      publicOperandBinding(
          Execution.Receipt.Guarantees.CandidatePublishOperandBinding));
}

bool validCompletePublicationReceipt(
    const publication::SanitizerPublicationExecutionResultV1 &Execution) {
  if (!Execution.succeeded() ||
      Execution.Receipt.Version !=
          publication::kSanitizerPublicationExecutorSchemaVersion ||
      Execution.Receipt.Platform != publication::PublicationPlatform::MacOS)
    return false;
  const auto &Guarantees = Execution.Receipt.Guarantees;
  switch (Execution.Receipt.NamespaceDisposition) {
  case publication::PublicationNamespaceDisposition::CreateExclusive:
    return Execution.Outcome ==
               publication::SanitizerPublicationOutcome::Published &&
           Guarantees.NamespaceAtomic &&
           Guarantees.DestinationCreateExclusive &&
           !Guarantees.CompareAndSwap && !Guarantees.CrashDurable &&
           Guarantees.CandidatePublishOperandBinding !=
               publication::CandidatePublishOperandBindingV1::None;
  case publication::PublicationNamespaceDisposition::NoChange:
    return Execution.Outcome ==
               publication::SanitizerPublicationOutcome::NotPublished &&
           !Guarantees.NamespaceAtomic &&
           !Guarantees.DestinationCreateExclusive &&
           !Guarantees.CompareAndSwap && !Guarantees.CrashDurable &&
           Guarantees.CandidatePublishOperandBinding ==
               publication::CandidatePublishOperandBindingV1::None;
  case publication::PublicationNamespaceDisposition::None:
    return false;
  }
  return false;
}

int publishAuthenticated(Session *S, const std::filesystem::path &SourcePath,
                         const std::filesystem::path &OutputPath,
                         std::vector<uint8_t> CandidateBytes,
                         uint64_t GuardedSiteCount, PatchResult Candidate,
                         const Pipeline::ObfuscationCounts &Obfuscation,
                         neverd_sanitize_result_v1 *Result,
                         PublicationExceptionState &PublicationState) {
  // Finish every allocation that contributes to observable session state
  // before the adapter can reach its namespace linearization point.
  Candidate.OutputPath = OutputPath.string();
  publication::DarwinSanitizerPublicationRequestV1 Request;
  Request.SourcePath = SourcePath.string();
  Request.DestinationPath = Candidate.OutputPath;
  publication::ArtifactContentDigestV1 SourceContent;
  SourceContent.SHA256 =
      llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(S->Img.Raw));
  SourceContent.Size = static_cast<uint64_t>(S->Img.Raw.size());
  Request.ExpectedSourceContent = SourceContent;
  Request.CandidateBytes = std::move(CandidateBytes);
  Request.GuardedSiteCount = GuardedSiteCount;

  llvm::Expected<publication::PreparedDarwinSanitizerPublicationV1> Prepared =
      publication::prepareDarwinSanitizerPublicationV1(std::move(Request));
  if (!Prepared)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_PUBLISH_FAILED,
                "cannot prepare authenticated sanitizer publication: " +
                    llvm::toString(Prepared.takeError()));

  const publication::SanitizerPublicationMetadataPlanV1 Plan =
      publication::planSanitizerPublicationMetadata(Prepared->MetadataRequest);
  if (!Plan.ready()) {
    std::string Detail = "authenticated sanitizer publication plan rejected (";
    Detail += publication::toString(Plan.Reason);
    Detail += ")";
    if (Plan.Reason == publication::SanitizerPublicationMetadataReason::
                           ExistingDestinationCASUnsupported)
      Detail +=
          ": v1 does not support replacement CAS; choose a new output path";
    else if (Plan.Reason == publication::SanitizerPublicationMetadataReason::
                                SameSourceGuardedUnsupported)
      Detail +=
          ": guarded sanitizer output cannot mutate the loaded source; choose "
          "a new output path";
    return fail(S, Result, NEVERD_SANITIZE_STATUS_PUBLISH_FAILED, Detail);
  }

  switch (S->SanitizePublicationFault) {
  case Session::SanitizePublicationFaultForTesting::None:
    break;
  case Session::SanitizePublicationFaultForTesting::PublishIndeterminate:
    Prepared->Operations.PublishNoReplace = [] {
      return publication::NamespacePublishResultV1{
          publication::SanitizerPublicationOutcome::Indeterminate,
          "injected indeterminate publication result"};
    };
    break;
  case Session::SanitizePublicationFaultForTesting::
      PublishedFinalAuthenticationFailure: {
    auto Authenticate =
        std::move(Prepared->Operations.AuthenticatePublishedFinal);
    Prepared->Operations.AuthenticatePublishedFinal =
        [Authenticate = std::move(Authenticate)]() mutable
        -> llvm::Expected<publication::AuthenticatedPublishedFinalArtifactV1> {
      llvm::Expected<publication::AuthenticatedPublishedFinalArtifactV1>
          Observed = Authenticate();
      if (!Observed)
        return Observed.takeError();
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "injected published-final authentication failure");
    };
    break;
  }
  case Session::SanitizePublicationFaultForTesting::
      PublishedFinalizationFailure: {
    auto Finalize = std::move(Prepared->Operations.FinalizePublished);
    Prepared->Operations.FinalizePublished =
        [Finalize = std::move(Finalize)]() mutable -> llvm::Error {
      if (llvm::Error Error = Finalize())
        return Error;
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "injected published-final finalization failure");
    };
    break;
  }
  }

  auto PublishNoReplace = std::move(Prepared->Operations.PublishNoReplace);
  Prepared->Operations.PublishNoReplace = [PublishNoReplace =
                                               std::move(PublishNoReplace),
                                           &PublicationState]() mutable {
    PublicationState = PublicationExceptionState::Indeterminate;
    publication::NamespacePublishResultV1 Published = PublishNoReplace();
    switch (Published.Outcome) {
    case publication::SanitizerPublicationOutcome::NotPublished:
      PublicationState = PublicationExceptionState::NotPublished;
      break;
    case publication::SanitizerPublicationOutcome::Published:
      PublicationState = PublicationExceptionState::Published;
      break;
    case publication::SanitizerPublicationOutcome::Indeterminate:
      break;
    }
    return Published;
  };

  const publication::SanitizerPublicationExecutionResultV1 Execution =
      publication::executeSanitizerPublicationMetadata(Plan, Prepared->Binding,
                                                       Prepared->Operations);
  writePublicationExecutionResult(Result, Execution);
  if (!Execution.succeeded()) {
    neverd_sanitize_status_t Status = NEVERD_SANITIZE_STATUS_PUBLISH_FAILED;
    if (Execution.Outcome ==
        publication::SanitizerPublicationOutcome::Indeterminate)
      Status = NEVERD_SANITIZE_STATUS_PUBLISH_INDETERMINATE;
    else if (Execution.Outcome ==
             publication::SanitizerPublicationOutcome::Published)
      Status = NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE;
    if (S->SanitizePublicationFault ==
            Session::SanitizePublicationFaultForTesting::PublishIndeterminate ||
        S->SanitizePublicationFault ==
            Session::SanitizePublicationFaultForTesting::
                PublishedFinalAuthenticationFailure)
      throw std::runtime_error(
          "injected post-publication diagnostic construction exception");
    std::string Detail = "authenticated sanitizer publication failed (";
    Detail += publication::toString(Execution.Reason);
    Detail += ")";
    if (!Execution.Detail.empty())
      Detail += ": " + Execution.Detail;
    return fail(S, Result, Status, Detail);
  }
  if (!validCompletePublicationReceipt(Execution)) {
    WRITE_RESULT(Result, publication_receipt_complete, uint32_t{0});
    WRITE_RESULT(Result, publication_guarantee_flags, uint32_t{0});
    WRITE_RESULT(Result, publication_operand_binding,
                 uint32_t{NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_NONE});
    const neverd_sanitize_status_t Status =
        Execution.Outcome == publication::SanitizerPublicationOutcome::Published
            ? NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE
            : NEVERD_SANITIZE_STATUS_RECEIPT_MISMATCH;
    return fail(S, Result, Status,
                "authenticated sanitizer publication returned an inconsistent "
                "complete receipt");
  }

  S->LastPatch = std::move(Candidate);
  S->commitObfuscationCounts(Obfuscation);
  WRITE_RESULT(Result, ok, 1);
  WRITE_RESULT(Result, status,
               neverd_sanitize_status_t{NEVERD_SANITIZE_STATUS_OK});
  return 1;
}

int publishNoOp(Session *S, const std::filesystem::path &SourcePath,
                const std::filesystem::path &OutputPath,
                neverd_sanitize_result_v1 *Result,
                PublicationExceptionState &PublicationState) {
  PatchResult Committed;
  Committed.Success = true;
  WRITE_RESULT(Result, plan_version, safety::kRuntimeSanitizerPlanVersion);
  return publishAuthenticated(S, SourcePath, OutputPath, S->Img.Raw,
                              /*GuardedSiteCount=*/0, std::move(Committed), {},
                              Result, PublicationState);
}

int publishGuarded(Session *S, const std::filesystem::path &SourcePath,
                   const std::filesystem::path &OutputPath, uint32_t Strategy,
                   std::filesystem::perms SafePermissions,
                   const GuardedSecurityContext &Security,
                   std::unique_ptr<llvm::Module> Module,
                   llvm::ArrayRef<safety::RuntimeSanitizerGuard> Guards,
                   neverd_sanitize_result_v1 *Result,
                   PublicationExceptionState &PublicationState) {
  // These are the only pre-guard LLVM transformations permitted by the strict
  // workflow.  Counts stay local until the binary publication commits.
  symbolizeImageAbsolutePointers(*Module, S->Img);
  const Pipeline::ObfuscationCounts Obfuscation =
      S->runSessionObfuscation(*Module);

  // This must remain the final LLVM mutation before code generation.
  safety::RuntimeBoundsSanitizerResult GuardResult =
      safety::applyRuntimeBoundsSanitizer(Module, Guards);
  if (!GuardResult.Complete) {
    std::string Detail = "runtime guard insertion failed (";
    Detail += safety::toString(GuardResult.Error);
    Detail += ")";
    if (!GuardResult.Detail.empty())
      Detail += ": " + GuardResult.Detail;
    return fail(S, Result, NEVERD_SANITIZE_STATUS_GUARD_FAILED, Detail);
  }
  WRITE_RESULT(
      Result, guarded_functions,
      static_cast<uint64_t>(GuardResult.GuardedOriginalEntries.size()));

  ScopedTemporaryFile SourceSnapshot;
  ScopedPathTemporaryFile BackendTemporary;
  std::string Error;
  if (!SourceSnapshot.create(OutputPath, "source", Error) ||
      !SourceSnapshot.write(S->Img.Raw, Error) ||
      !SourceSnapshot.setPermissions(SafePermissions, Error) ||
      !BackendTemporary.create(OutputPath, "backend", Error))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_IO_FAILED, Error);

  if (!requireBytes(SourceSnapshot.path(), S->Img.Raw, Error,
                    "before backend snapshot read"))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED, Error);

  PatchResult Candidate;
  if (Strategy == NEVERD_SANITIZE_STRATEGY_INPLACE) {
    auto Rewriter = InplaceRewriter::create(S->Img.Format);
    if (!Rewriter)
      return fail(S, Result, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET,
                  "inplace sanitizer rewrite is unsupported for this format");
    if (!S->TextSectionOverride.empty())
      Rewriter->setTextSectionOverride(S->TextSectionOverride);
    Candidate =
        Rewriter->rewrite(SourceSnapshot.path(), BackendTemporary.path(),
                          *Module, S->Img, S->Img.Arch);
  } else {
    auto Patcher = BinaryPatcher::create(S->Img.Format);
    if (!Patcher)
      return fail(S, Result, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET,
                  "section sanitizer patching is unsupported for this format");
    Patcher->setImageContext(&S->Img);
    if (!S->TextSectionOverride.empty())
      Patcher->setTextSectionOverride(S->TextSectionOverride);
    Candidate = Patcher->patch(SourceSnapshot.path(), BackendTemporary.path(),
                               *Module, S->Img.Arch);
  }
  if (!Candidate.Success)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_PATCH_FAILED,
                "sanitizer backend did not produce a complete patch");
  if (!BackendTemporary.setPermissions(SafePermissions, Error))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_IO_FAILED, Error);
  if (!requireBytes(SourceSnapshot.path(), S->Img.Raw, Error,
                    "after backend snapshot read"))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED, Error);

  if (S->SanitizeAfterBackendForTesting)
    S->SanitizeAfterBackendForTesting(BackendTemporary.path());

  const bool SignedMachO = Security.MachO && !isUnsignedMachO(*Security.MachO);
  if (SignedMachO) {
#ifdef __APPLE__
    const std::string &Identifier = Security.MachO->Slices.front().Identifier;
    if (llvm::Error Signing = macho_signing::adHocResign(
            BackendTemporary.path().string(), Identifier))
      return fail(S, Result, NEVERD_SANITIZE_STATUS_SIGNING_FAILED,
                  llvm::toString(std::move(Signing)));
#else
    return fail(S, Result, NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED,
                "signed Mach-O publication requires the macOS codesign tool");
#endif
  }

  // External signing is allowed to replace only this backend candidate's
  // inode.  Capture its finalized bytes; the native publication adapter later
  // stages those exact in-memory bytes in its own held candidate inode.
  std::vector<uint8_t> CandidateBytes;
  if (!readBytes(BackendTemporary.path(), CandidateBytes, Error))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_IO_FAILED,
                "cannot capture patched candidate bytes: " + Error);
  if (llvm::Error Validation =
          validateGuardedSecurityBytes(Security, CandidateBytes))
    return fail(S, Result,
                SignedMachO ? NEVERD_SANITIZE_STATUS_SIGNING_FAILED
                            : NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED,
                llvm::toString(std::move(Validation)));
  if (SignedMachO) {
#ifdef __APPLE__
    if (llvm::Error Verify =
            macho_signing::verifyStrict(BackendTemporary.path().string()))
      return fail(S, Result, NEVERD_SANITIZE_STATUS_SIGNING_FAILED,
                  llvm::toString(std::move(Verify)));
#endif
  }
  if (!requirePermissions(BackendTemporary.path(), SafePermissions, Error,
                          "after platform finalization"))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED, Error);
  if (CandidateBytes == S->Img.Raw)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_RECEIPT_MISMATCH,
                "guarded patch receipt produced byte-identical output");
  if (S->SanitizeBeforeReloadForTesting)
    S->SanitizeBeforeReloadForTesting(BackendTemporary.path());
  if (!requireBytes(BackendTemporary.path(), CandidateBytes, Error,
                    "before candidate reload"))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED, Error);

  WRITE_RESULT(Result, patched_functions,
               static_cast<uint64_t>(Candidate.PatchedOriginalEntries.size()));
  WRITE_RESULT(Result, code_size, static_cast<uint64_t>(Candidate.CodeSize));
  WRITE_RESULT(Result, trampoline_count,
               static_cast<uint64_t>(Candidate.TrampolineCount));
  if (!std::is_sorted(Candidate.PatchedOriginalEntries.begin(),
                      Candidate.PatchedOriginalEntries.end()) ||
      std::adjacent_find(Candidate.PatchedOriginalEntries.begin(),
                         Candidate.PatchedOriginalEntries.end()) !=
          Candidate.PatchedOriginalEntries.end())
    return fail(S, Result, NEVERD_SANITIZE_STATUS_RECEIPT_MISMATCH,
                "patch receipt is not sorted and unique");
  for (va_t GuardedEntry : GuardResult.GuardedOriginalEntries)
    if (!std::binary_search(Candidate.PatchedOriginalEntries.begin(),
                            Candidate.PatchedOriginalEntries.end(),
                            GuardedEntry))
      return fail(S, Result, NEVERD_SANITIZE_STATUS_RECEIPT_MISMATCH,
                  "patch receipt is missing guarded function entry " +
                      vaHex(GuardedEntry));

  auto Reloaded = loadBinary(BackendTemporary.path());
  if (!Reloaded)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_RELOAD_FAILED,
                "cannot reload sanitizer candidate: " +
                    llvm::toString(Reloaded.takeError()));
  if (Reloaded->Raw != CandidateBytes)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED,
                "reloaded candidate bytes differ from the patch receipt "
                "artifact");
  if (!authenticateReload(S->Img, *Reloaded, Error))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED, Error);

  if (S->SanitizeBeforePublishForTesting)
    S->SanitizeBeforePublishForTesting(BackendTemporary.path());
  if (!requireBytes(BackendTemporary.path(), CandidateBytes, Error,
                    "immediately before publication"))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED, Error);
  return publishAuthenticated(
      S, SourcePath, OutputPath, std::move(CandidateBytes), Guards.size(),
      std::move(Candidate), Obfuscation, Result, PublicationState);
}

} // namespace

extern "C" {

const char *neverd_sanitize_status_name(neverd_sanitize_status_t Status) {
  switch (Status) {
  case NEVERD_SANITIZE_STATUS_OK:
    return "ok";
  case NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT:
    return "invalid-argument";
  case NEVERD_SANITIZE_STATUS_INVALID_SESSION:
    return "invalid-session";
  case NEVERD_SANITIZE_STATUS_NOT_LOADED:
    return "not-loaded";
  case NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET:
    return "unsupported-target";
  case NEVERD_SANITIZE_STATUS_PIPELINE_FAILED:
    return "pipeline-failed";
  case NEVERD_SANITIZE_STATUS_INCOMPLETE_COVERAGE:
    return "incomplete-coverage";
  case NEVERD_SANITIZE_STATUS_HUNT_INCOMPLETE:
    return "hunt-incomplete";
  case NEVERD_SANITIZE_STATUS_METADATA_INVALID:
    return "metadata-invalid";
  case NEVERD_SANITIZE_STATUS_PLAN_INCOMPLETE:
    return "plan-incomplete";
  case NEVERD_SANITIZE_STATUS_GUARD_FAILED:
    return "guard-failed";
  case NEVERD_SANITIZE_STATUS_IO_FAILED:
    return "io-failed";
  case NEVERD_SANITIZE_STATUS_PATCH_FAILED:
    return "patch-failed";
  case NEVERD_SANITIZE_STATUS_RECEIPT_MISMATCH:
    return "receipt-mismatch";
  case NEVERD_SANITIZE_STATUS_RELOAD_FAILED:
    return "reload-failed";
  case NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED:
    return "authentication-failed";
  case NEVERD_SANITIZE_STATUS_PUBLISH_FAILED:
    return "publish-failed";
  case NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED:
    return "signature-unsupported";
  case NEVERD_SANITIZE_STATUS_SIGNING_FAILED:
    return "signing-failed";
  case NEVERD_SANITIZE_STATUS_PUBLISH_INDETERMINATE:
    return "publish-indeterminate";
  case NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE:
    return "published-incomplete";
  }
  return "invalid";
}

uint32_t neverd_sanitize_publication_abi_version(void) {
  return NEVERD_SANITIZE_PUBLICATION_ABI_VERSION;
}

static int sanitizeImpl(neverd_session_t Sess, const char *OutputPath,
                        const neverd_sanitize_options_v1 *Options,
                        neverd_sanitize_result_v1 *Result,
                        PublicationExceptionState &PublicationState) {
  clearResult(Result);
  initializePublicationResult(Result);
  auto *S = toSession(Sess);
  if (!Result ||
      !reaches(Result->struct_size,
               FIELD_END(neverd_sanitize_result_v1, status)) ||
      splitsKnownResultField(Result->struct_size)) {
    if (S) {
      S->clearError();
      S->setError("sanitize result struct_size is invalid");
    }
    return 0;
  }
  if (!Sess) {
    Result->status = NEVERD_SANITIZE_STATUS_INVALID_SESSION;
    return 0;
  }
  S->clearError();
  if (!Options) {
    return fail(S, Result, NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT,
                "sanitize options are null");
  }
  if (Options->struct_size < sizeof(size_t) ||
      splitsKnownOptionsField(Options->struct_size))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT,
                "sanitize options struct_size is invalid");
  const uint32_t Strategy =
      reaches(Options->struct_size,
              FIELD_END(neverd_sanitize_options_v1, strategy))
          ? Options->strategy
          : NEVERD_SANITIZE_STRATEGY_SECTION;
  if (Strategy != NEVERD_SANITIZE_STRATEGY_SECTION &&
      Strategy != NEVERD_SANITIZE_STRATEGY_INPLACE)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT,
                "sanitize strategy is invalid");
  if (!OutputPath || OutputPath[0] == '\0')
    return fail(S, Result, NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT,
                "sanitize output path is empty");
  if (!S->Loaded)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_NOT_LOADED,
                "no binary loaded");
  if (!isNativeTarget(S->Img))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET,
                "strict sanitizer supports native PE, ELF, and Mach-O only");

  if (S->Img.IsRelocatable)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET,
                "relocatable object sanitizer publication is unsupported");

  std::vector<uint8_t> CurrentSource;
  std::string Error;
  const std::filesystem::path SourcePath = S->SanitizeSourcePath;
  if (SourcePath.empty())
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED,
                "loaded session has no authenticated sanitizer source "
                "locator");
  if (!readBytes(SourcePath, CurrentSource, Error))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED,
                "cannot authenticate loaded source: " + Error);
  llvm::Expected<NativeImageKind> ImageKind =
      classifyNativeImage(CurrentSource);
  if (!ImageKind)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED,
                "cannot authenticate native image kind: " +
                    llvm::toString(ImageKind.takeError()));
  if (*ImageKind == NativeImageKind::UniversalMachO)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET,
                "universal Mach-O publication is unsupported");
  if (*ImageKind == NativeImageKind::Relocatable)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET,
                "relocatable object sanitizer publication is unsupported");
  if (*ImageKind == NativeImageKind::DynamicLibrary)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET,
                "dynamic library sanitizer publication is unsupported");
  if (CurrentSource != S->Img.Raw)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED,
                "loaded source bytes changed after session load");
  std::filesystem::perms SafePermissions;
  if (!captureSafePermissions(SourcePath, SafePermissions, Error))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED, Error);

  const std::filesystem::path RequestedDestination(OutputPath);
  std::filesystem::path Destination;
  bool DestinationIsBundleMember = false;
  if (!normalizePublicationDestination(RequestedDestination, Destination,
                                       DestinationIsBundleMember, Error))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT, Error);
  if (S->SanitizeAfterDestinationResolutionForTesting)
    S->SanitizeAfterDestinationResolutionForTesting(Destination);
  if (DestinationIsBundleMember)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED,
                "sanitizer destination inside a signed bundle is "
                "unsupported");
#ifndef __APPLE__
  return fail(
      S, Result, NEVERD_SANITIZE_STATUS_UNSUPPORTED_TARGET,
      "authenticated sanitizer publication is unsupported on this host; "
      "Darwin is required");
#endif
  llvm::LLVMContext Context;
  PipelineOptions PipelineOpts;
  PipelineOpts.PatchMode = true;
  PipelineOpts.NoOpt = true;
  PipelineOpts.MaxFunctions = 0;
  S->applyAnalysisOptions(PipelineOpts);
  Pipeline ThePipeline;
  PipelineResult PipelineRun =
      ThePipeline.run(S->Img, Context, PipelineOpts, S->Dbg.get());
  if (!PipelineRun.Success || !PipelineRun.LlvmModule ||
      PipelineRun.SourceImage != &S->Img ||
      PipelineRun.MedIRVerifierFailures != 0 ||
      PipelineRun.BackendUnhandledValueIntrinsics != 0 ||
      PipelineRun.LLVMVerifierFailed)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_PIPELINE_FAILED,
                PipelineRun.Error.empty()
                    ? "strict sanitizer pipeline did not produce a complete "
                      "verified LLVM module"
                    : PipelineRun.Error);
  if (std::optional<std::string> Coverage =
          safety::validatePipelineCoverage(PipelineRun, &S->Img))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_INCOMPLETE_COVERAGE,
                *Coverage);

  safety::AnalysisInput Analysis;
  Analysis.Img = &S->Img;
  Analysis.MedFuncs = &PipelineRun.MedFuncs;
  Analysis.LowFuncs = &PipelineRun.LowFuncs;
  Analysis.ValidatedPipeline = &PipelineRun;
  Analysis.Dbg = S->Dbg.get();
  Analysis.DebugKind = S->DbgKind;
  Analysis.Renames = &S->Renames;
  const auto SignatureNames = S->SigDB.buildNameMap();
  Analysis.SignatureNames = &SignatureNames;
  const TargetRegInfo &Registers = getTargetRegInfo(S->Img.Arch);
  Analysis.StackPointerReg = Registers.StackPointer;
  Analysis.FramePointerReg = Registers.FramePointer;
  Analysis.StackRegsKnown = true;

  const safety::SafetyReport Hunt = safety::runHunt(
      Analysis, safety::SinkCatalog::defaults(), readBudgets(Options));
  if (!Hunt.AnalysisComplete || Hunt.BudgetHit || !Hunt.Error.empty())
    return fail(S, Result, NEVERD_SANITIZE_STATUS_HUNT_INCOMPLETE,
                Hunt.Error.empty()
                    ? (Hunt.BudgetHit
                           ? "strict safety hunt exhausted a configured budget"
                           : "strict safety hunt was incomplete")
                    : Hunt.Error);

  if (S->Img.Format == BinaryFormat::MachO && !Hunt.Findings.empty()) {
    bool SourceIsBundleMember = false;
    if (!isBundleMember(SourcePath, SourceIsBundleMember, Error))
      return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED,
                  Error);
    if (SourceIsBundleMember)
      return fail(S, Result, NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED,
                  "guarded Mach-O bundle member publication is unsupported");
  }

  std::vector<safety_callsite_md::SafetyCallsiteRecord> Records;
  if (!collectCallsiteRecords(*PipelineRun.LlvmModule, Records, Error))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_METADATA_INVALID, Error);
  safety::RuntimeSanitizerPlan Plan =
      safety::planRuntimeSanitizer(S->Img.Format, Hunt.Findings, Records);

  WRITE_RESULT(Result, plan_version, Plan.Version);
  WRITE_RESULT(Result, findings, static_cast<uint64_t>(Hunt.Findings.size()));
  WRITE_RESULT(Result, guarded_sites,
               static_cast<uint64_t>(Plan.Guards.size()));
  WRITE_RESULT(Result, guarded_functions,
               static_cast<uint64_t>(uniqueGuardedFunctions(Plan.Guards)));
  WRITE_RESULT(Result, unsupported_sites,
               static_cast<uint64_t>(Plan.Unsupported.size()));
  if (!Plan.Complete || Plan.Guards.size() != Hunt.Findings.size()) {
    std::string Detail = "strict sanitizer plan is incomplete";
    if (!Plan.Unsupported.empty()) {
      Detail += " (";
      Detail += safety::toString(Plan.Unsupported.front().Reason);
      Detail += "): ";
      Detail += Plan.Unsupported.front().Detail;
    }
    return fail(S, Result, NEVERD_SANITIZE_STATUS_PLAN_INCOMPLETE, Detail);
  }

  if (Plan.Guards.empty())
    return publishNoOp(S, SourcePath, Destination, Result, PublicationState);

  std::error_code DestinationExistsError;
  const bool DestinationExists =
      std::filesystem::exists(Destination, DestinationExistsError);
  if (DestinationExistsError)
    return fail(S, Result, NEVERD_SANITIZE_STATUS_IO_FAILED,
                "cannot inspect sanitizer destination: " +
                    DestinationExistsError.message());
  if (DestinationExists) {
    std::error_code EquivalentError;
    const bool Equivalent =
        std::filesystem::equivalent(SourcePath, Destination, EquivalentError);
    if (EquivalentError)
      return fail(S, Result, NEVERD_SANITIZE_STATUS_IO_FAILED,
                  "cannot authenticate source/output path identity: " +
                      EquivalentError.message());
    if (Equivalent)
      return fail(
          S, Result, NEVERD_SANITIZE_STATUS_INVALID_ARGUMENT,
          "guarded sanitizer output must not replace the loaded source; "
          "publish to a distinct path");
  }

  GuardedSecurityContext Security;
  ScopedPathTemporaryFile VerificationSnapshot;
  neverd_sanitize_status_t SecurityFailure =
      NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED;
  if (!prepareGuardedSecurity(S, SourcePath, CurrentSource, Destination,
                              SafePermissions, VerificationSnapshot, Security,
                              SecurityFailure, Error))
    return fail(S, Result, SecurityFailure, Error);
  if (!requireBytes(SourcePath, CurrentSource, Error,
                    "after signature preflight"))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_AUTHENTICATION_FAILED, Error);
  if (!VerificationSnapshot.removeBeforeCommit(Error))
    return fail(S, Result, NEVERD_SANITIZE_STATUS_IO_FAILED,
                "cannot remove source signature snapshot: " + Error);

  return publishGuarded(S, SourcePath, Destination, Strategy, SafePermissions,
                        Security, std::move(PipelineRun.LlvmModule),
                        Plan.Guards, Result, PublicationState);
}

int neverd_session_sanitize(neverd_session_t Sess, const char *OutputPath,
                            const neverd_sanitize_options_v1 *Options,
                            neverd_sanitize_result_v1 *Result) {
  PublicationExceptionState PublicationState =
      PublicationExceptionState::NotInvoked;
  try {
    return sanitizeImpl(Sess, OutputPath, Options, Result, PublicationState);
  } catch (const std::exception &Exception) {
    return failUnexpected(Sess, Result, Exception.what(), PublicationState);
  } catch (...) {
    return failUnexpected(Sess, Result,
                          "unexpected native sanitizer transaction failure",
                          PublicationState);
  }
}

} // extern "C"
