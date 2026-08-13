//===- NeverDCAPIRoundtrip.cpp - C API: lift-to-object verification -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lift-to-object roundtrip and semantic-verification result queries.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/backend/codegen/CodeGen.h"

using namespace neverd;
using namespace neverd::sdk;

int neverd_lift_to_obj(neverd_session_t Sess, const char *InputPath, int NoOpt,
                       int MaxFunctions) {
  auto *S = static_cast<Session *>(Sess);
  if (!S)
    return 1;
  S->RoundTrip.Valid = false;

  PipelineRunner R;
  std::string Err;
  if (!R.load(InputPath, Err)) {
    if (S)
      S->setError(Err);
    return 1;
  }
  if (R.Img.Arch == Arch::EVM || R.Img.Arch == Arch::SBF) {
    S->setError(
        "object-code roundtrip is not supported for virtual-machine inputs");
    return 1;
  }

  PipelineOptions Opts;
  Opts.LiftMode = true;
  Opts.NoOpt = NoOpt != 0;
  Opts.MaxFunctions = MaxFunctions > 0 ? static_cast<size_t>(MaxFunctions) : 0;
  if (!R.run(Opts, Err)) {
    if (S)
      S->setError(Err);
    return 1;
  }
  if (!R.Result.LlvmModule) {
    if (S)
      S->setError("no LLVM module produced");
    return 1;
  }

  {
    std::string Buf;
    llvm::raw_string_ostream OS(Buf);
    R.Result.LlvmModule->print(OS, nullptr);
    S->RoundTrip.IR = std::move(Buf);
  }

  S->RoundTrip.ParamCounts.resize(R.Result.MedFuncs.size());
  for (size_t I = 0; I < R.Result.MedFuncs.size(); ++I)
    S->RoundTrip.ParamCounts[I] =
        static_cast<int>(R.Result.MedFuncs[I].Params.size());

  Codegen CG;
  S->RoundTrip.CG =
      CG.compile(*R.Result.LlvmModule, R.Img.Arch, BinaryFormat::ELF);
  if (!S->RoundTrip.CG.Success) {
    if (S)
      S->setError("codegen failed");
    return 1;
  }

  S->RoundTrip.Valid = true;
  return 0;
}

const char *neverd_roundtrip_ir(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->RoundTrip.Valid)
    return nullptr;
  return dupStr(S->RoundTrip.IR);
}

const unsigned char *neverd_roundtrip_obj(neverd_session_t Sess,
                                          unsigned long long *OutLen) {
  auto *S = toSession(Sess);
  if (!S->RoundTrip.Valid || S->RoundTrip.CG.ObjectData.empty()) {
    if (OutLen)
      *OutLen = 0;
    return nullptr;
  }
  if (OutLen)
    *OutLen = S->RoundTrip.CG.ObjectData.size();
  return S->RoundTrip.CG.ObjectData.data();
}

int neverd_roundtrip_func_count(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S->RoundTrip.Valid)
    return 0;
  return static_cast<int>(S->RoundTrip.CG.Functions.size());
}

const char *neverd_roundtrip_func_name(neverd_session_t Sess, int Index) {
  auto *S = toSession(Sess);
  if (!S->RoundTrip.Valid || Index < 0 ||
      Index >= (int)S->RoundTrip.CG.Functions.size())
    return nullptr;
  return dupStr(S->RoundTrip.CG.Functions[Index].Name);
}

unsigned long long neverd_roundtrip_func_offset(neverd_session_t Sess,
                                                int Index) {
  auto *S = toSession(Sess);
  if (!S->RoundTrip.Valid || Index < 0 ||
      Index >= (int)S->RoundTrip.CG.Functions.size())
    return 0;
  return S->RoundTrip.CG.Functions[Index].Offset;
}

unsigned long long neverd_roundtrip_func_size(neverd_session_t Sess,
                                              int Index) {
  auto *S = toSession(Sess);
  if (!S->RoundTrip.Valid || Index < 0 ||
      Index >= (int)S->RoundTrip.CG.Functions.size())
    return 0;
  return S->RoundTrip.CG.Functions[Index].Size;
}

int neverd_roundtrip_func_param_count(neverd_session_t Sess, int Index) {
  auto *S = toSession(Sess);
  if (!S->RoundTrip.Valid || Index < 0 ||
      Index >= (int)S->RoundTrip.ParamCounts.size())
    return 0;
  return S->RoundTrip.ParamCounts[Index];
}
