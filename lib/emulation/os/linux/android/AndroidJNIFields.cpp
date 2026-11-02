//===- AndroidJNIFields.cpp - Supplied DEX field access -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

namespace neverd::emulation::android_model {
BionicResult Bionic::jniFields(unsigned Slot, const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Receiver = jniObject(A[1]);
  if (!Receiver)
    return Receiver.takeError();
  if (!*Receiver)
    return failure("JNI field access requires a nonnull receiver/class");
  if (!JNI.DexRuntime)
    return unsupportedImport(Call);
  const bool Static = Slot >= 144;
  if (Slot == 94 || Slot == 144) {
    if ((*Receiver)->Type != JNIObject::Class)
      return failure("JNI field lookup requires a class reference");
    auto Name = string(A[2]);
    if (!Name)
      return Name.takeError();
    auto Signature = string(A[3]);
    if (!Signature)
      return Signature.takeError();
    const auto Owner = "L" + (*Receiver)->ClassName + ";";
    return executeDex(
        nullptr, {}, Owner,
        [this, Owner, Static, Name = *Name,
         Signature = *Signature](uint64_t) -> BionicResult {
          const auto *Field =
              JNI.Dex->findField({Owner, Name, Signature}, Static);
          if (!Field) {
            Result.Stop = ProcessStopReason::UnsupportedService;
            Result.Diagnostic = "JNI field declaration not supplied";
            return std::nullopt;
          }
          for (auto [ID, Existing] : JNI.DexFields)
            if (Existing == Field)
              return value(ID);
          if (JNI.DexFields.size() >= jni_abi::ReferenceLimit)
            return failure("JNI DEX field identity budget exhausted");
          const uint64_t ID =
              jni_abi::Base + 0x1200000 + JNI.DexFields.size() * 8;
          JNI.DexFields.emplace(ID, Field);
          return value(ID);
        });
  }
  auto It = JNI.DexFields.find(A[2]);
  if (It == JNI.DexFields.end())
    return failure("JNI field ID was not resolved by this VM");
  const auto &Field = *It->second;
  unsigned Offset = Slot - (Static ? 145 : 95);
  const bool Write = Offset >= 9;
  const char Kind = "LZBCSIJFD"[Offset % 9];
  const auto &Type = Field.reference.type;
  if (Static != mobile::dalvik::has(Field.access, "static") ||
      (Kind == 'L' ? !mobile::dalvik::referenceType(Type)
                   : Type != std::string(1, Kind)))
    return failure("JNI field accessor type or storage kind mismatch");
  if (Kind == 'F' || Kind == 'D')
    return unsupportedImport(Call);
  if (Static && ((*Receiver)->Type != JNIObject::Class ||
                 !JNI.Dex->assignable("L" + (*Receiver)->ClassName + ";",
                                      Field.reference.owner)))
    return failure("JNI static field class mismatch");
  uint64_t Bits = A[3];
  if (Write && Kind == 'L') {
    auto Object = jniObject(Bits);
    if (!Object)
      return Object.takeError();
    Bits = *Object ? (*Object)->Identity : 0;
  } else if (Write && Kind == 'Z')
    Bits = uint8_t(Bits);
  return runDex(
      [&](const auto &Admit, uint64_t Stack) {
        return JNI.DexRuntime->field(Field, Static ? 0 : (*Receiver)->Identity,
                                     Write ? std::optional<uint64_t>(Bits)
                                           : std::nullopt,
                                     Admit, Stack, threadID());
      },
      [this, Write, Kind](uint64_t Bits) -> BionicResult {
        if (Write)
          return value(0);
        return Kind == 'L' ? dexReference(Bits) : value(Bits);
      });
}
} // namespace neverd::emulation::android_model
