//===- ProcessProbe.cpp - Direct bounded process SDK driver --------------===//
// NeverD Decompiler
/// \file
/// Run the process SDK with an explicit profile and preserve its typed stop.
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>

int main(int Argc, char **Argv) {
  using namespace neverd::emulation;
  if (Argc != 5) {
    llvm::errs() << "usage: neverd-process-probe binary profile options.json "
                    "report.json\n";
    return 2;
  }
  auto Fail = [](llvm::Error E) {
    llvm::errs() << llvm::toString(std::move(E)) << '\n';
    return 1;
  };
  auto Profile = parseProcessProfile(Argv[2]);
  if (!Profile)
    return Fail(Profile.takeError());
  auto Text = llvm::MemoryBuffer::getFile(Argv[3]);
  if (!Text)
    return Fail(llvm::errorCodeToError(Text.getError()));
  auto Options = processOptionsFromJSON((*Text)->getBuffer());
  if (!Options)
    return Fail(Options.takeError());
  auto Result = emulateProcess(Argv[1], *Profile, *Options);
  if (!Result)
    return Fail(Result.takeError());
  std::ofstream Out(Argv[4]);
  if (!Out)
    return 1;
  {
    llvm::raw_os_ostream OS(Out);
    writeProcessResultJSON(OS, *Result);
    OS << '\n';
  }
  Out.close();
  if (!Out)
    return 1;
  llvm::errs() << processStopReasonName(Result->Stop) << ": "
               << Result->Diagnostic << '\n';
  return 0;
}
