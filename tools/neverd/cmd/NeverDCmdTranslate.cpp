//===- NeverDCmdTranslate.cpp - The translate-object subcommand ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `neverd translate-object` compiles the published fail-closed x86-64 v1
/// scalar-register subset to an audited AArch64 relocatable object through the
/// public C API.  It accepts only canonical encodings without legacy prefixes:
/// REX.W full-width GPR MOV, ADD/SUB, and register/immediate AND/OR/XOR forms;
/// full-width register-only CMP 39/3B and register/immediate CMP 81/7, 83/7,
/// and 3D; and full-width register-only TEST 85 and register/immediate TEST
/// F7/0 and A9 over supported LowIR shapes. Arithmetic forms retain their
/// scalar flag computations; logical and TEST forms compute
/// architecture-defined flags while preserving AF in the NeverD state model.
/// Canonical C3 RET or C2 iw RET imm16 terminates a return block, and
/// direct-relative EB cb or E9 cd JMP terminates a direct-branch block. The
/// published lowering schema is 9. Canonical, legacy-prefix-free traditional
/// Jcc comprises JO/JNO 70/71 or 0F 80/81, JB/JAE 72/73 or 0F 82/83, JE/JNE
/// 74/75 or 0F 84/85, JBE/JA 76/77 or 0F 86/87, JS/JNS 78/79 or 0F 88/89,
/// JP/JNP 7A/7B or 0F 8A/8B, JL/JGE 7C/7D or 0F 8C/8D, and JLE/JG 7E/7F or 0F
/// 8E/8F, with cb short or cd near displacements respectively. JRCXZ/JECXZ/JCXZ
/// and LOOP/LOOPE/LOOPNE remain unpublished and fail closed. Reserved F7 /1,
/// ordinary guest memory, partial registers, legacy prefixes, semantically
/// redundant REX extension bits, any instruction or encoding outside that exact
/// subset, all other control flow, unimplemented LowIR, and bytes after the
/// block terminator are rejected. The command stops at the object boundary: no
/// linker, loader, publisher, dispatcher, execution engine, or debugger is
/// involved.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"
#include "../NeverDTranslateOutput.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

using namespace llvm;

namespace neverd::cli {

namespace {

enum ExitStatus : int {
  ExitSuccess = 0,
  ExitInvalidInput = 2,
  ExitEngineFailure = 4,
};

class TranslationResultGuard {
public:
  explicit TranslationResultGuard(neverd_translate_object_result_v1 &Value)
      : Value(Value) {}
  ~TranslationResultGuard() { neverd_translate_object_result_dispose(&Value); }

  TranslationResultGuard(const TranslationResultGuard &) = delete;
  TranslationResultGuard &operator=(const TranslationResultGuard &) = delete;

private:
  neverd_translate_object_result_v1 &Value;
};

bool writeObjectAtomically(StringRef Path, ArrayRef<uint8_t> Bytes) {
  Expected<sys::fs::TempFile> TempOrErr = sys::fs::TempFile::create(
      Path + ".tmp-%%%%%%", sys::fs::all_read | sys::fs::all_write,
      sys::fs::OF_None);
  if (!TempOrErr) {
    WithColor::error() << "cannot create temporary output for " << Path << ": "
                       << toString(TempOrErr.takeError()) << "\n";
    return false;
  }

  sys::fs::TempFile Temp = std::move(*TempOrErr);
  bool WriteFailed = false;
  {
    raw_fd_ostream Stream(Temp.FD, false);
    Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Stream.flush();
    WriteFailed = Stream.has_error();
  }
  if (WriteFailed) {
    const std::string TempName = Temp.TmpName;
    Error DiscardError = detail::discardTemporaryOutput(Temp);
    WithColor::error() << "cannot finish writing " << Path;
    if (DiscardError)
      errs() << " (temporary cleanup failed for " << TempName << ": "
             << toString(std::move(DiscardError)) << ")";
    errs() << "\n";
    return false;
  }

  if (Error CommitError = detail::closeAndCommitTemporaryOutput(Temp, Path)) {
    WithColor::error() << "cannot commit " << Path << ": "
                       << toString(std::move(CommitError)) << "\n";
    return false;
  }
  return true;
}

} // namespace

int runTranslateObject() {
  std::optional<uint64_t> Entry = parseAddrArg(TranslateObjectEntry);
  if (!Entry) {
    WithColor::error() << "invalid guest entry address: "
                       << TranslateObjectEntry << "\n";
    return ExitInvalidInput;
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> Buffer =
      MemoryBuffer::getFile(TranslateObjectInput, /*IsText=*/false,
                            /*RequiresNullTerminator=*/false);
  if (!Buffer) {
    WithColor::error() << "cannot read " << TranslateObjectInput << ": "
                       << Buffer.getError().message() << "\n";
    return ExitInvalidInput;
  }

  const StringRef GuestBytes = (*Buffer)->getBuffer();
  neverd_translate_object_request_v1 Request{};
  Request.struct_size = sizeof(Request);
  Request.guest_bytes =
      reinterpret_cast<const unsigned char *>(GuestBytes.data());
  Request.guest_bytes_size = GuestBytes.size();
  Request.entry_pc = *Entry;
  Request.executable_generation = TranslateObjectGeneration;
  Request.object_format =
      TranslateObjectFormat.getValue() == TranslateObjectContainer::ELF
          ? NEVERD_TRANSLATE_OBJECT_FORMAT_ELF
          : NEVERD_TRANSLATE_OBJECT_FORMAT_MACHO;

  neverd_translate_object_result_v1 Result{};
  Result.struct_size = sizeof(Result);
  TranslationResultGuard ResultGuard(Result);
  const int CallStatus =
      neverd_translate_x86_64_block_to_aarch64_object_v1(&Request, &Result);
  if (CallStatus != 0) {
    WithColor::error() << "translation API refused the result contract\n";
    return ExitEngineFailure;
  }
  if (!Result.ok) {
    WithColor::error() << "translation failed ["
                       << neverd_translate_error_code_name(Result.error_code)
                       << "]";
    if (Result.error_message && Result.error_message[0] != '\0')
      errs() << ": " << Result.error_message;
    errs() << "\n";
    return Result.error_code == NEVERD_TRANSLATE_ERROR_INVALID_ARGUMENT
               ? ExitInvalidInput
               : ExitEngineFailure;
  }
  if (Result.error_code != NEVERD_TRANSLATE_ERROR_NONE) {
    WithColor::error()
        << "translation returned success with a non-success error category\n";
    return ExitEngineFailure;
  }
  if (Error BindingError =
          detail::validateTranslationResultBindingV1(Request, Result)) {
    WithColor::error() << toString(std::move(BindingError)) << "\n";
    return ExitEngineFailure;
  }
  if (Result.object_format != Request.object_format) {
    WithColor::error() << "translation returned an unexpected object format\n";
    return ExitEngineFailure;
  }
  if (Result.guest_byte_count != GuestBytes.size()) {
    WithColor::error()
        << "translation did not consume the complete input block\n";
    return ExitEngineFailure;
  }
  if (!Result.object_bytes || Result.object_size == 0) {
    WithColor::error() << "translation returned an empty object\n";
    return ExitEngineFailure;
  }

  const ArrayRef<uint8_t> ObjectBytes(Result.object_bytes, Result.object_size);
  return writeObjectAtomically(TranslateObjectOutput, ObjectBytes)
             ? ExitSuccess
             : ExitEngineFailure;
}

} // namespace neverd::cli
