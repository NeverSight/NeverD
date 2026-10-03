//===- WindowsProcessVariables.cpp - Guest process environment -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcess.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation::windows_process {
namespace {
using namespace value;
struct Variable {
  std::u16string Name, Value;
};
std::optional<std::string> key(const std::u16string &Name) {
  std::string Key;
  for (char16_t C : Name) {
    if (C > UINT8_MAX || !llvm::isPrint(char(C)))
      return std::nullopt;
    Key += llvm::toLower(char(C));
  }
  return Key;
}
} // namespace

llvm::Expected<std::u16string> Services::readWide(uint64_t Address,
                                                  uint64_t Limit) {
  std::u16string Text;
  for (uint64_t I = 0; I < Limit; ++I) {
    if (!Budget.remainingMicroseconds())
      return failure(text::EnvironmentTimeout);
    if (Address >= UserLimit || WideSize > UserLimit - Address)
      return failure(text::Access);
    auto Readable = access(Address, WideSize, Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return failure(text::Access);
    auto Unit = CPU.readInteger(Address, WideSize);
    if (!Unit)
      return Unit.takeError();
    if (!*Unit)
      return Text;
    Text += char16_t(*Unit);
    Address += WideSize;
  }
  return failure(text::EnvironmentLimit);
}
llvm::Error Services::writeWide(uint64_t Address, const std::u16string &Text) {
  const uint64_t Size = (Text.size() + 1) * WideSize;
  auto Writable = access(Address, Size, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return failure(text::UserException);
  std::vector<uint8_t> Bytes(Size);
  for (size_t I = 0; I < Text.size(); ++I)
    llvm::support::endian::write16le(Bytes.data() + I * WideSize, Text[I]);
  return CPU.write(Address, Bytes);
}
llvm::Expected<std::optional<uint64_t>>
Services::environment(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto WinError = [&](uint32_t Code,
                      uint64_t Value =
                          0) -> llvm::Expected<std::optional<uint64_t>> {
    auto V = error(Code, Value);
    if (!V)
      return V.takeError();
    return std::optional<uint64_t>(*V);
  };
  if (S.Kind == API::FreeEnvironmentStringsW) {
    auto Found = Allocations.find(A[0]);
    if (Found == Allocations.end() || !Found->second.EnvironmentSnapshot)
      return unsupported(S);
    if (auto E = Memory.unmap(Found->first, Found->second.MappedSize))
      return std::move(E);
    Allocations.erase(Found);
    return std::optional<uint64_t>(1);
  }

  // These pointers establish the admitted allocation's identity. Read the
  // actual guest block, never a second host-side variable database.
  for (auto [Address, Expected] :
       {std::pair{TEB + TebPEB, PEB},
        std::pair{PEB + PebParameters, Parameters},
        std::pair{Parameters + ParamsEnvironment, Env.Variables}}) {
    auto Readable = access(Address, PointerSize, Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return failure(text::Access);
    auto Pointer = CPU.readInteger(Address, PointerSize);
    if (!Pointer)
      return Pointer.takeError();
    if (*Pointer != Expected)
      return failure(text::EnvironmentChanged);
  }
  std::map<std::string, Variable> Variables;
  std::u16string Block;
  uint64_t Offset = 0;
  while (true) {
    auto Item = readWide(Env.Variables + Offset * WideSize,
                         EnvironmentCapacity / WideSize - Offset);
    if (!Item)
      return Item.takeError();
    Offset += Item->size() + 1;
    if (Item->empty()) {
      if (Block.empty()) {
        auto Tail = readWide(Env.Variables + WideSize, 1);
        if (!Tail)
          return Tail.takeError();
        Block += u'\0';
      }
      break;
    }
    const size_t Equal = Item->find(u'=');
    if (!Equal || Equal == std::u16string::npos)
      return failure(text::EnvironmentChanged);
    auto Name = Item->substr(0, Equal);
    auto Key = key(Name);
    if (!Key ||
        !Variables
             .emplace(*Key, Variable{std::move(Name), Item->substr(Equal + 1)})
             .second)
      return failure(text::EnvironmentChanged);
    Block += *Item;
    Block += u'\0';
  }
  if (S.Kind == API::GetEnvironmentStringsW) {
    auto Address = allocateHeap((Block.size() + 1) * WideSize, true);
    if (!Address)
      return Address.takeError();
    if (!*Address)
      return WinError(ErrorNotEnoughMemory);
    if (auto E = writeWide(*Address, Block))
      return std::move(E);
    return std::optional<uint64_t>(*Address);
  }

  if (S.Kind == API::ExpandEnvironmentStringsW) {
    auto Source = readWide(A[0], MaxStringUnits);
    if (!Source)
      return Source.takeError();
    std::u16string Expanded;
    for (size_t I = 0; I < Source->size();) {
      if (!Budget.remainingMicroseconds())
        return failure(text::EnvironmentTimeout);
      size_t End = (*Source)[I] == u'%' ? Source->find(u'%', I + 1)
                                        : std::u16string::npos;
      if ((*Source)[I] != u'%' || End == std::u16string::npos) {
        if (Expanded.size() + 1 >= MaxStringUnits)
          return failure(text::EnvironmentLimit);
        Expanded += (*Source)[I++];
      } else {
        auto Key = key(Source->substr(I + 1, End - I - 1));
        if (!Key)
          return unsupported(S);
        auto Found = Variables.find(*Key);
        const auto Replacement = Found == Variables.end()
                                     ? Source->substr(I, End - I + 1)
                                     : Found->second.Value;
        if (Replacement.size() >= MaxStringUnits - Expanded.size())
          return failure(text::EnvironmentLimit);
        Expanded += Replacement;
        I = End + 1;
      }
    }
    const uint64_t Needed = Expanded.size() + 1;
    if (uint32_t(A[2]) >= Needed) {
      const uint64_t SourceSize = (Source->size() + 1) * WideSize;
      const uint64_t OutputSize = Needed * WideSize;
      if (A[1] >= UserLimit || OutputSize > UserLimit - A[1])
        return failure(text::UserException);
      if (A[0] < A[1] + OutputSize && A[1] < A[0] + SourceSize)
        return unsupported(S);
      if (auto E = writeWide(A[1], Expanded))
        return std::move(E);
    }
    return std::optional<uint64_t>(Needed);
  }

  if (!A[0]) {
    if (S.Kind == API::SetEnvironmentVariableW)
      return failure(text::UserException);
    return WinError(ErrorEnvironmentNotFound);
  }
  auto Name = readWide(A[0], MaxStringUnits);
  if (!Name)
    return Name.takeError();
  if (Name->empty() || Name->find(u'=') != std::u16string::npos)
    return WinError(S.Kind == API::SetEnvironmentVariableW
                        ? ErrorInvalidParameter
                        : ErrorEnvironmentNotFound);
  auto Key = key(*Name);
  if (!Key)
    return unsupported(S);
  auto Found = Variables.find(*Key);
  if (S.Kind == API::GetEnvironmentVariableW) {
    if (Found == Variables.end())
      return WinError(ErrorEnvironmentNotFound);
    const auto &Value = Found->second.Value;
    if (uint32_t(A[2]) <= Value.size())
      return std::optional<uint64_t>(Value.size() + 1);
    if (auto E = writeWide(A[1], Value))
      return std::move(E);
    return std::optional<uint64_t>(Value.size());
  }
  if (A[1]) {
    auto Value = readWide(A[1], MaxStringUnits);
    if (!Value)
      return Value.takeError();
    // Replacement preserves the original name's spelling.
    if (Found != Variables.end())
      Found->second.Value = std::move(*Value);
    else
      Variables.emplace(*Key, Variable{*Name, std::move(*Value)});
  } else {
    // Deleting an absent variable succeeds without changing LastError or the
    // environment block, as observed by the independent native fixture.
    if (Found == Variables.end())
      return std::optional<uint64_t>(1);
    Variables.erase(Found);
  }
  Block.clear();
  for (const auto &[Key, V] : Variables) {
    if (V.Name.size() + V.Value.size() + 2 >
        EnvironmentCapacity / WideSize - 1 - Block.size())
      return failure(text::EnvironmentLimit);
    Block += V.Name;
    Block += u'=';
    Block += V.Value;
    Block += u'\0';
  }
  if (Block.empty())
    Block += u'\0';
  // All inputs, capacity and output pages are validated before the write.
  if (auto E = writeWide(Env.Variables, Block))
    return std::move(E);
  return std::optional<uint64_t>(1);
}
} // namespace neverd::emulation::windows_process
