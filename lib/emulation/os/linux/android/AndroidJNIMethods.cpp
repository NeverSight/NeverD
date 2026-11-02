//===- AndroidJNIMethods.cpp - Bounded Java intrinsic execution -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// JNI method identities and value-based Java operations. Unknown method
/// bodies remain unsupported; resolving a name never supplies a canned result.
//===----------------------------------------------------------------------===//
#include "../../../arch/aarch64/AArch64FloatConversion.h"
#include "AndroidArguments.h"

#include "neverd/dalvik/Library.h"
#include "neverd/emulation/Limits.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <bit>

namespace neverd::emulation::android_model {
namespace {
using namespace mobile::dalvik;
using Method = LibraryMethod;
const auto Methods = libraryMethods();
constexpr uint64_t MethodBase = jni_abi::Base + 0x1000000;
constexpr uint64_t DexMethodBase = MethodBase + 0x100000;
std::string objectClass(const JNIObject &Object) {
  return Object.Type == JNIObject::Class ? "java/lang/Class" : Object.ClassName;
}
// Descriptors have been validated by the intrinsic table or shared DEX reader.
std::vector<char> parameters(llvm::StringRef Signature) {
  std::vector<char> Types;
  for (size_t I = 1; Signature[I] != ')';) {
    char Type = Signature[I++];
    if (Type == '[') {
      while (Signature[I] == '[')
        ++I;
      Type = Signature[I++];
      if (Type == 'L')
        I = Signature.find(';', I) + 1;
      Type = 'L';
    } else if (Type == 'L') {
      I = Signature.find(';', I) + 1;
    }
    Types.push_back(Type);
  }
  return Types;
}
} // namespace

BionicResult Bionic::jniMethods(unsigned Slot, const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Receiver = jniObject(A[1]);
  if (!Receiver)
    return Receiver.takeError();
  if (!*Receiver)
    return failure("JNI method operation requires a nonnull receiver/class");
  if (Slot == 33 || Slot == 113) {
    if ((*Receiver)->Type != JNIObject::Class)
      return failure("JNI method lookup requires a class reference");
    auto Name = string(A[2]);
    if (!Name)
      return Name.takeError();
    auto Signature = string(A[3]);
    if (!Signature)
      return Signature.takeError();
    const auto ClassName = (*Receiver)->ClassName;
    auto Resolve = [this, Slot, ClassName, Name = *Name, Signature = *Signature,
                    CallName = Call.Name](uint64_t) -> BionicResult {
      try {
        auto [Params, Return] = mobile::dalvik::prototype(Signature);
        mobile::dalvik::MethodRef Ref{"L" + ClassName + ";", Name,
                                      std::move(Params), std::move(Return)};
        if (Ref.name.starts_with('<') && !(Slot == 33 && Ref.name == "<init>"))
          return failure("JNI cannot explicitly invoke a DEX initializer");
        if (const auto *Body = JNI.Dex->findMethod(Ref, Slot == 113)) {
          for (auto [ID, Existing] : JNI.DexMethods)
            if (Existing == Body)
              return value(ID);
          if (JNI.DexMethods.size() >= jni_abi::ReferenceLimit)
            return failure("JNI DEX method identity budget exhausted");
          uint64_t ID = DexMethodBase + JNI.DexMethods.size() * 8;
          JNI.DexMethods.emplace(ID, Body);
          return value(ID);
        }
      } catch (const mobile::Error &E) {
        return failure(E.what());
      }
      for (unsigned I = std::size(Methods); I-- > 0;) {
        const auto &M = Methods[I];
        if (Name != M.Name || Signature != M.Signature ||
            M.Static != (Slot == 113) || !jniAssignable(ClassName, M.Class) ||
            ((M.Op == Construct || M.Op == ThrowableConstruct ||
              M.Op == ObjectConstruct) &&
             ClassName != M.Class))
          continue;
        JNI.ResolvedMethods.insert(I);
        return value(MethodBase + I * 8);
      }
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = diagnostic::ImportPrefix + CallName +
                          " (unmodeled Java method " + ClassName + "." + Name +
                          Signature + ")";
      return std::nullopt;
    };
    const auto Owner = "L" + ClassName + ";";
    if (JNI.Dex->containsClass(Owner))
      return executeDex(nullptr, {}, Owner, std::move(Resolve));
    return Resolve(0);
  }
  const bool New = Slot >= 28 && Slot <= 30;
  const bool Nonvirtual = Slot >= 64 && Slot <= 93;
  const bool Static = Slot >= 114;
  const unsigned First = Nonvirtual ? 4 : 3;
  const uint64_t ID = A[First - 1];
  const mobile::dalvik::Method *DexMethod = nullptr;
  std::string DexClass, DexSignature;
  Method M{};
  if (auto It = JNI.DexMethods.find(ID); It != JNI.DexMethods.end()) {
    DexMethod = It->second;
    DexClass = DexMethod->reference.owner.substr(
        1, DexMethod->reference.owner.size() - 2);
    DexSignature = DexMethod->reference.signature();
    M = {DexClass.c_str(), DexMethod->reference.name.c_str(),
         DexSignature.c_str(),
         DexMethod->reference.name == "<init>" ? Construct : GetClass,
         mobile::dalvik::has(DexMethod->access, "static")};
  } else {
    if (ID < MethodBase || (ID - MethodBase) % 8 ||
        (ID - MethodBase) / 8 >= std::size(Methods) ||
        !JNI.ResolvedMethods.contains(unsigned((ID - MethodBase) / 8)))
      return failure("JNI method ID was not resolved by this VM");
    M = Methods[(ID - MethodBase) / 8];
  }
  llvm::StringRef Signature = M.Signature;
  const char Return = Signature[Signature.find(')') + 1];
  const unsigned Base = New ? 28 : Static ? 114 : Nonvirtual ? 64 : 34;
  const unsigned Form = (Slot - Base) % 3;
  const char RequestedReturn = New ? 'V' : "LZBCSIJFDV"[(Slot - Base) / 3];
  if (M.Static != Static ||
      (New != ((M.Op == Construct || M.Op == ThrowableConstruct ||
                M.Op == ObjectConstruct))) ||
      RequestedReturn != (Return == '[' ? 'L' : Return))
    return failure("JNI method invocation kind or return type mismatch");
  if (Static || New) {
    if ((*Receiver)->Type != JNIObject::Class ||
        (New ? (*Receiver)->ClassName != M.Class
             : !jniAssignable((*Receiver)->ClassName, M.Class)))
      return failure("JNI static method/constructor class mismatch");
  } else if (!jniAssignable(objectClass(**Receiver), M.Class)) {
    return failure("JNI method receiver type mismatch");
  }
  if (Nonvirtual) {
    auto Class = jniObject(A[2]);
    if (!Class)
      return Class.takeError();
    if (!*Class || (*Class)->Type != JNIObject::Class ||
        !jniAssignable(objectClass(**Receiver), (*Class)->ClassName) ||
        !jniAssignable((*Class)->ClassName, M.Class))
      return failure("JNI nonvirtual method class mismatch");
    if (DexMethod) {
      auto Lookup = DexMethod->reference;
      Lookup.owner = "L" + (*Class)->ClassName + ";";
      if (JNI.Dex->findMethod(Lookup, false) != DexMethod)
        return failure("JNI nonvirtual method ID does not match class lookup");
    }
    if (!DexMethod && JNI.Dex) {
      auto [Params, ReturnType] = mobile::dalvik::prototype(M.Signature);
      mobile::dalvik::MethodRef Lookup{"L" + (*Class)->ClassName + ";", M.Name,
                                       Params, ReturnType};
      if (JNI.Dex->findMethod(Lookup, false))
        return failure(
            "JNI nonvirtual intrinsic ID does not match class lookup");
    }
    // An inherited ID cannot select a different override in the named class.
    if (M.Op == IdentityEquals && (*Class)->ClassName != M.Class) {
      auto [Params, ResultType] = prototype(M.Signature);
      if (libraryMethod({"L" + (*Class)->ClassName + ";", M.Name,
                         std::move(Params), std::move(ResultType)},
                        false))
        return failure("JNI nonvirtual method ID does not match class lookup");
    }
  }
  const auto Types = parameters(Signature);
  if (Return == 'F' || Return == 'D' ||
      (DexMethod &&
       std::find(Types.begin(), Types.end(), 'F') != Types.end()) ||
      std::find(Types.begin(), Types.end(), 'D') != Types.end())
    return unsupportedImport(Call);
  std::vector<uint64_t> Values;
  uint64_t Remaining =
      ::neverd::limits::emulation::kAndroidJNIMethodsBionicJniMethodsRemaining;
  ArgumentReader Arguments(*this, Call, First, Remaining);
  if (Form == 1 && !Types.empty())
    if (auto E = Arguments.initializeVAList(A[First]))
      return std::move(E);
  if (Form == 2 && !Types.empty())
    if (auto E = access(A[First], Types.size() * 8, Read))
      return std::move(E);
  for (unsigned I = 0; I < Types.size(); ++I) {
    uint64_t V;
    if (Form == 2) {
      uint8_t Bytes[8];
      if (auto E = CPU.read(A[First] + I * 8, Bytes))
        return std::move(E);
      V = llvm::support::endian::read64le(Bytes);
    } else if (Types[I] == 'F') {
      auto Argument = Arguments.nextFloating();
      if (!Argument)
        return Argument.takeError();
      auto Control = CPU.readRegister(CPURegister::AArch64FPCR);
      if (!Control)
        return Control.takeError();
      const auto Converted = narrowAArch64Double(*Argument, (*Control)[0]);
      if (!Converted) {
        Result.Stop = ProcessStopReason::UnsupportedService;
        Result.Diagnostic =
            "JNI promoted float conversion has unsupported FPCR controls";
        return std::optional<BionicValue>();
      }
      if (Converted->Raised) {
        auto Status = CPU.readRegister(CPURegister::AArch64FPSR);
        if (!Status)
          return Status.takeError();
        (*Status)[0] |= Converted->Raised;
        if (auto E = CPU.writeRegister(CPURegister::AArch64FPSR, *Status))
          return std::move(E);
      }
      V = Converted->Bits;
    } else {
      auto Argument =
          Arguments.next(Types[I] == 'L' || Types[I] == 'J' ? 8 : 4);
      if (!Argument)
        return Argument.takeError();
      V = *Argument;
    }
    // Narrow primitives consume their declared bits, regardless of carrier.
    if (Types[I] == 'Z')
      V = uint8_t(V);
    if (Types[I] == 'C')
      V = uint16_t(V);
    if (Types[I] == 'B')
      V = uint32_t(int32_t(std::bit_cast<int8_t>(uint8_t(V))));
    if (Types[I] == 'S')
      V = uint32_t(int32_t(std::bit_cast<int16_t>(uint16_t(V))));
    if (Types[I] == 'I' || Types[I] == 'F')
      V = uint32_t(V);
    Values.push_back(V);
  }
  for (unsigned I = 0; I < Values.size(); ++I)
    if (Types[I] == 'L') {
      auto Object = jniObject(Values[I]);
      if (!Object)
        return Object.takeError();
      Values[I] = *Object ? (*Object)->Identity : 0;
    }
  if (DexMethod) {
    if (New) {
      return allocateDex(DexMethod->reference.owner, false,
                         [this, DexMethod, Values = std::move(Values)](
                             uint64_t Instance) mutable {
                           Values.insert(Values.begin(), Instance);
                           return executeDex(DexMethod, Values, {},
                                             [this, Instance](uint64_t) {
                                               return dexReference(Instance);
                                             });
                         });
    }
    if (!Static)
      Values.insert(Values.begin(), (*Receiver)->Identity);
    if (!Static && !Nonvirtual &&
        !mobile::dalvik::has(DexMethod->access, "private")) {
      DexMethod = JNI.Dex->virtualMethod(
          DexMethod->reference, mobile::dalvik::objectDescriptor(**Receiver));
      if (!DexMethod)
        return failure("JNI virtual method has no supplied body");
    }
    return executeDex(DexMethod, Values, {}, [this, Return](uint64_t Bits) {
      return Return == 'L' || Return == '[' ? dexReference(Bits) : value(Bits);
    });
  }
  if (JNI.Dex && !Static && !New && !Nonvirtual) {
    auto [Params, ResultType] = mobile::dalvik::prototype(M.Signature);
    mobile::dalvik::MethodRef Ref{"L" + std::string(M.Class) + ";", M.Name,
                                  Params, ResultType};
    if (auto Body = JNI.Dex->virtualMethod(
            Ref, mobile::dalvik::objectDescriptor(**Receiver))) {
      Values.insert(Values.begin(), (*Receiver)->Identity);
      return executeDex(Body, Values, {}, [this, Return](uint64_t Bits) {
        return Return == 'L' || Return == '[' ? dexReference(Bits)
                                              : value(Bits);
      });
    }
  }
  auto Executed = runDex([&](const auto &Admit, uint64_t) {
    return JNI.DexRuntime->library(M, Static || New ? 0 : (*Receiver)->Identity,
                                   Values, Admit, Nonvirtual);
  });
  if (!Executed || !*Executed || JNI.Threads.at(threadID()).Exception)
    return Executed;
  if (New || Return == 'L' || Return == '[') {
    auto Ref = jniReference(JNI.Heap->get(std::get<uint64_t>(**Executed)));
    if (!Ref)
      return Ref.takeError();
    return value(*Ref);
  }
  return Executed;
}
} // namespace neverd::emulation::android_model
