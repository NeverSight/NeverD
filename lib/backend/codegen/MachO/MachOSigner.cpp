//===- MachOSigner.cpp - Controlled Darwin signing subprocess -----------===//

#include "neverd/backend/codegen/MachO/MachOSigner.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::macho_signing {

namespace {

constexpr llvm::StringLiteral CodesignPath = "/usr/bin/codesign";
constexpr unsigned CodesignTimeoutSeconds = 30;
constexpr size_t MaximumDetailLength = 256;

llvm::Error signerError(llvm::StringRef Detail) {
  return llvm::make_error<llvm::StringError>("macho signing: " + Detail,
                                             llvm::inconvertibleErrorCode());
}

std::string boundedDetail(llvm::StringRef Detail) {
  Detail = Detail.take_front(MaximumDetailLength);
  std::string Result = Detail.str();
  for (char &Character : Result)
    if (static_cast<unsigned char>(Character) < 0x20)
      Character = ' ';
  return Result;
}

ProcessResult executeProduction(const ProcessRequest &Request) {
  llvm::SmallVector<llvm::StringRef, 10> Arguments;
  Arguments.reserve(Request.Arguments.size());
  for (const std::string &Argument : Request.Arguments)
    Arguments.emplace_back(Argument);

  llvm::SmallVector<llvm::StringRef, 4> EnvironmentStorage;
  EnvironmentStorage.reserve(Request.Environment.size());
  for (const std::string &Entry : Request.Environment)
    EnvironmentStorage.emplace_back(Entry);
  const std::optional<llvm::ArrayRef<llvm::StringRef>> Environment =
      Request.InheritEnvironment
          ? std::nullopt
          : std::optional<llvm::ArrayRef<llvm::StringRef>>(EnvironmentStorage);

  std::array<std::optional<llvm::StringRef>, 3> Redirects = {
      llvm::StringRef(), llvm::StringRef(), llvm::StringRef()};
  const llvm::ArrayRef<std::optional<llvm::StringRef>> RedirectView =
      Request.DiscardStandardStreams
          ? llvm::ArrayRef<std::optional<llvm::StringRef>>(Redirects)
          : llvm::ArrayRef<std::optional<llvm::StringRef>>();
  ProcessResult Result;
  Result.ExitCode = llvm::sys::ExecuteAndWait(
      Request.Program, Arguments, Environment, RedirectView,
      Request.TimeoutSeconds, 0, &Result.Detail, &Result.ExecutionFailed);
  return Result;
}

llvm::Error checkResult(llvm::StringRef Operation,
                        const ProcessResult &Result) {
  const std::string Detail = boundedDetail(Result.Detail);
  std::string Message = "codesign ";
  Message += Operation;
  if (Result.ExecutionFailed || Result.ExitCode == -1) {
    Message += " failed to launch";
  } else if (Result.ExitCode == -2) {
    std::string Lower = Detail;
    std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](char Value) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(Value)));
    });
    Message += llvm::StringRef(Lower).contains("timed out")
                   ? " timed out"
                   : " terminated by a signal";
  } else if (Result.ExitCode != 0) {
    Message += " exited with status ";
    Message += std::to_string(Result.ExitCode);
  } else {
    return llvm::Error::success();
  }
  if (!Detail.empty()) {
    Message += ": ";
    Message += Detail;
  }
  return signerError(Message);
}

llvm::Error execute(llvm::StringRef Operation,
                    std::vector<std::string> Arguments, const Operations &Ops) {
  if (!Ops.Execute)
    return signerError("codesign executor is unavailable");
  ProcessRequest Request;
  Request.Program = CodesignPath.str();
  Request.Arguments = std::move(Arguments);
  Request.TimeoutSeconds = CodesignTimeoutSeconds;
  Request.DiscardStandardStreams = true;
  // /usr/bin/codesign does not need PATH, locale, a keychain, or allocator
  // discovery for identityless signing.  An explicit empty environment keeps
  // CODESIGN_ALLOCATE and DYLD_* from redirecting trusted subprocess behavior.
  Request.Environment.clear();
  Request.InheritEnvironment = false;
  return checkResult(Operation, Ops.Execute(Request));
}

bool invalidArgument(llvm::StringRef Value) {
  return Value.empty() || Value.contains('\0');
}

} // namespace

const Operations &productionOperations() {
  static const Operations Ops{executeProduction};
  return Ops;
}

llvm::Error verifyStrict(llvm::StringRef Path, const Operations &Ops) {
  if (invalidArgument(Path))
    return signerError("codesign verification path is empty or invalid");
  return execute("verification",
                 {CodesignPath.str(), "--verify", "--strict", Path.str()}, Ops);
}

llvm::Error adHocResign(llvm::StringRef Path, llvm::StringRef Identifier,
                        const Operations &Ops) {
  if (invalidArgument(Path))
    return signerError("codesign output path is empty or invalid");
  if (invalidArgument(Identifier))
    return signerError("codesign identifier is empty or invalid");
  return execute("signing",
                 {CodesignPath.str(), "--force", "--sign", "-", "--identifier",
                  Identifier.str(), "--timestamp=none", Path.str()},
                 Ops);
}

} // namespace neverd::macho_signing
