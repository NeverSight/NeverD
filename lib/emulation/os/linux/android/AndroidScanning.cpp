//===- AndroidScanning.cpp - Bounded Bionic byte scanning -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Implements bounded Android integer and narrow character/string scanning.
//===----------------------------------------------------------------------===//
#include "AndroidArguments.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <limits>

namespace neverd::emulation::android_model {
namespace {
constexpr bool isSpace(char C) { return C == ' ' || (C >= '\t' && C <= '\r'); }

unsigned digit(char C) {
  if (C >= '0' && C <= '9')
    return C - '0';
  if (C >= 'a' && C <= 'f')
    return C - 'a' + 10;
  if (C >= 'A' && C <= 'F')
    return C - 'A' + 10;
  return 16;
}

bool overlaps(uint64_t A, uint64_t ASize, uint64_t B, uint64_t BSize) {
  return A <= B ? B - A < ASize : A - B < BSize;
}
} // namespace

/// Independently implements bounded integer and byte sscanf/vsscanf
/// conversions. Parse and validate before publishing writes. Matching failures
/// still commit the earlier assignments, while unsupported behavior produces no
/// result.
class Bionic::IntegerScanner {
public:
  IntegerScanner(Bionic &Model, const NativeCallEvent &Call)
      : Model(Model), Call(Call), Remaining(Model.Options.MemoryLimit),
        Arguments(Model, Call, 2, Remaining) {}

  llvm::Expected<uint64_t> run();
  bool isUnsupported() const { return Unsupported; }

private:
  struct Directive {
    char Conversion;
    unsigned Size = 4;
    uint64_t Width = 0;
    bool Suppressed = false;
    llvm::StringRef Literal;
  };
  struct Assignment {
    uint64_t Address;
    llvm::SmallVector<uint8_t, 8> Bytes;
  };
  Bionic &Model;
  const NativeCallEvent &Call;
  uint64_t Remaining;
  ArgumentReader Arguments;
  bool Unsupported = false;
  std::string Input, Format;
  llvm::SmallVector<Directive, 8> Directives;
  llvm::SmallVector<Assignment, 8> Assignments;
  size_t Position = 0;
  uint32_t Count = 0;

  llvm::Error unsupported(llvm::StringRef Reason) {
    Unsupported = true;
    return failure(Call.Name + ": " + Reason);
  }
  llvm::Error reserve(uint64_t Bytes) {
    if (Bytes > Remaining)
      return failure(diagnostic::ScanLimit);
    Remaining -= Bytes;
    return llvm::Error::success();
  }
  llvm::Expected<std::string> text(uint64_t Address);
  llvm::Error parse();
  llvm::Expected<bool> number(const Directive &D);
  llvm::Error characters(const Directive &D);
  llvm::Error assign(const Directive &D, uint64_t Value);
  llvm::Error assign(const Directive &D, llvm::ArrayRef<uint8_t> Bytes,
                     bool Terminated = false);
  llvm::Expected<uint64_t> match();
  llvm::Error commit();
};

llvm::Expected<std::string> Bionic::IntegerScanner::text(uint64_t Address) {
  std::string Text;
  for (;;) {
    if (auto E = reserve(1))
      return std::move(E);
    if (Address > UINT64_MAX - Text.size())
      return failure(diagnostic::GuestPointer);
    auto Byte = Model.byte(Address + Text.size());
    if (!Byte)
      return Byte.takeError();
    if (!*Byte)
      return Text;
    Text.push_back(*Byte);
  }
}

llvm::Error Bionic::IntegerScanner::parse() {
  llvm::StringRef Rest = Format;
  while (!Rest.empty()) {
    if (auto E = Model.access(0, 0, Read))
      return E;
    if (auto E = reserve(sizeof(Directive)))
      return E;
    Directive D{Rest.front()};
    if (D.Conversion != '%') {
      size_t Length = 1;
      const bool Space = isSpace(Rest.front());
      while (Length < Rest.size() && Rest[Length] != '%' &&
             isSpace(Rest[Length]) == Space)
        ++Length;
      D.Conversion = Space ? ' ' : '=';
      D.Literal = Rest.take_front(Length);
      Rest = Rest.drop_front(Length);
      Directives.push_back(D);
      continue;
    }
    Rest = Rest.drop_front();
    if (Rest.consume_front("%")) {
      D.Conversion = '=';
      D.Literal = "%";
      Directives.push_back(D);
      continue;
    }
    D.Suppressed = Rest.consume_front("*");
    const bool HasWidth = !Rest.empty() && digit(Rest.front()) < 10;
    while (!Rest.empty() && digit(Rest.front()) < 10) {
      unsigned Digit = digit(Rest.front());
      if (D.Width > (UINT64_MAX - Digit) / 10)
        return unsupported(diagnostic::ScanFormat);
      D.Width = D.Width * 10 + Digit;
      Rest = Rest.drop_front();
    }
    bool HasLength = true;
    if (Rest.consume_front("hh"))
      D.Size = 1;
    else if (Rest.consume_front("h"))
      D.Size = 2;
    else if (Rest.consume_front("ll") || Rest.consume_front("l") ||
             Rest.consume_front("j") || Rest.consume_front("z") ||
             Rest.consume_front("t") || Rest.consume_front("q"))
      D.Size = 8;
    else
      HasLength = false;
    if (Rest.empty())
      return unsupported(diagnostic::ScanFormat);
    D.Conversion = Rest.front();
    Rest = Rest.drop_front();
    if (D.Conversion == 'D' || D.Conversion == 'O') {
      D.Size = 8;
      D.Conversion += 'a' - 'A';
    }
    if (!llvm::StringRef("diouxXpncs").contains(D.Conversion) ||
        (llvm::StringRef("pcs").contains(D.Conversion) && HasLength) ||
        (D.Conversion == 'n' && (HasWidth || D.Suppressed)))
      return unsupported(diagnostic::ScanFormat);
    if (D.Conversion == 'p')
      D.Size = 8;
    Directives.push_back(D);
  }
  return llvm::Error::success();
}

llvm::Error Bionic::IntegerScanner::assign(const Directive &D, uint64_t Value) {
  std::array<uint8_t, 8> Bytes{};
  llvm::support::endian::write64le(Bytes.data(), Value);
  return assign(D, llvm::ArrayRef(Bytes).take_front(D.Size));
}

llvm::Error Bionic::IntegerScanner::assign(const Directive &D,
                                           llvm::ArrayRef<uint8_t> Bytes,
                                           bool Terminated) {
  if (D.Suppressed)
    return llvm::Error::success();
  auto Address = Arguments.next();
  if (!Address)
    return Address.takeError();
  if (auto E = reserve(sizeof(Assignment)))
    return E;
  const uint64_t Size = Bytes.size() + uint64_t(Terminated);
  if (auto E = reserve(Size))
    return E;
  if (auto E = Model.access(*Address, Size, Write))
    return E;
  Assignment A{*Address, {}};
  A.Bytes.append(Bytes.begin(), Bytes.end());
  if (Terminated)
    A.Bytes.push_back(0);
  Assignments.push_back(std::move(A));
  if (D.Conversion != 'n')
    ++Count;
  return llvm::Error::success();
}

llvm::Error Bionic::IntegerScanner::characters(const Directive &D) {
  const bool Terminated = D.Conversion == 's';
  llvm::StringRef Field = llvm::StringRef(Input).drop_front(Position);
  const uint64_t Width = D.Width ? D.Width : Terminated ? Field.size() : 1;
  Field = Field.take_front(Width);
  if (Terminated)
    Field = Field.take_until(isSpace);
  Position += Field.size();
  return assign(D,
                llvm::ArrayRef(reinterpret_cast<const uint8_t *>(Field.data()),
                               Field.size()),
                Terminated);
}

llvm::Expected<bool> Bionic::IntegerScanner::number(const Directive &D) {
  // Bionic uses a 513-byte numeric buffer, including the final NUL.
  const uint64_t Width = D.Width ? std::min(D.Width, uint64_t(512)) : 512;
  llvm::StringRef Field =
      llvm::StringRef(Input).drop_front(Position).take_front(Width);
  size_t Cursor = 0;
  bool Negative = Field.front() == '-';
  if (Negative || Field.front() == '+')
    ++Cursor;
  const bool Signed = D.Conversion == 'd' || D.Conversion == 'i';
  unsigned Base = D.Conversion == 'o'                          ? 8
                  : D.Conversion == 'd' || D.Conversion == 'u' ? 10
                  : D.Conversion == 'i'                        ? 0
                                                               : 16;
  if (!Base)
    Base = Cursor < Field.size() && Field[Cursor] == '0' ? 8 : 10;
  // An incomplete 0x prefix consumes only its zero. A width can make an
  // otherwise valid prefix incomplete, including after a sign.
  if ((Base == 16 || D.Conversion == 'i') && Cursor + 2 < Field.size() &&
      Field[Cursor] == '0' &&
      (Field[Cursor + 1] == 'x' || Field[Cursor + 1] == 'X') &&
      digit(Field[Cursor + 2]) < 16) {
    Base = 16;
    Cursor += 2;
  }
  const size_t Start = Cursor;
  uint64_t Value = 0;
  const uint64_t Limit = Signed ? uint64_t(INT64_MAX) + Negative : UINT64_MAX;
  while (Cursor < Field.size() && digit(Field[Cursor]) < Base) {
    unsigned Digit = digit(Field[Cursor++]);
    if (D.Suppressed)
      continue;
    if (Value > (Limit - Digit) / Base)
      return unsupported(diagnostic::ScanOverflow);
    Value = Value * Base + Digit;
  }
  if (Cursor == Start)
    return false;
  Position += Cursor;
  if (auto E = assign(D, Negative ? uint64_t(0) - Value : Value))
    return std::move(E);
  return true;
}

llvm::Expected<uint64_t> Bionic::IntegerScanner::match() {
  auto InputFailure = [&]() -> uint64_t { return Count ? Count : UINT32_MAX; };
  for (const auto &D : Directives) {
    if (auto E = Model.access(0, 0, Read))
      return std::move(E);
    if (D.Conversion == 'n') {
      if (auto E = assign(D, Position))
        return std::move(E);
      continue;
    }
    if (D.Conversion == '=') {
      for (char C : D.Literal) {
        if (Position == Input.size())
          return InputFailure();
        if (Input[Position] != C)
          return Count;
        ++Position;
      }
      continue;
    }
    if (D.Conversion != 'c')
      while (Position < Input.size() && isSpace(Input[Position]))
        ++Position;
    if (D.Conversion == ' ')
      continue;
    if (Position == Input.size())
      return InputFailure();
    if (D.Conversion == 'c' || D.Conversion == 's') {
      if (auto E = characters(D))
        return std::move(E);
      continue;
    }
    auto Matched = number(D);
    if (!Matched)
      return Matched.takeError();
    if (!*Matched)
      return Count;
  }
  return Count;
}

llvm::Error Bionic::IntegerScanner::commit() {
  // Input and format strings are read once, as is each consumed argument.
  // Reject writes that would invalidate those snapshots. Destination aliases
  // with each other remain valid and are applied in conversion order.
  for (const auto &A : Assignments)
    if (overlaps(A.Address, A.Bytes.size(), Call.Arguments[0],
                 Input.size() + 1) ||
        overlaps(A.Address, A.Bytes.size(), Call.Arguments[1],
                 Format.size() + 1) ||
        Arguments.overlaps(A.Address, A.Bytes.size()))
      return unsupported(diagnostic::ScanAlias);
  for (const auto &A : Assignments) {
    if (auto E = Model.CPU.write(A.Address, A.Bytes))
      return E;
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> Bionic::IntegerScanner::run() {
  auto Source = text(Call.Arguments[0]);
  if (!Source)
    return Source.takeError();
  Input = std::move(*Source);
  auto Pattern = text(Call.Arguments[1]);
  if (!Pattern)
    return Pattern.takeError();
  Format = std::move(*Pattern);
  if (auto E = parse())
    return std::move(E);
  if (Call.Name == symbol::Vsscanf)
    if (auto E = Arguments.initializeVAList(Call.Arguments[2]))
      return std::move(E);
  auto Value = match();
  if (!Value)
    return Value.takeError();
  if (auto E = commit())
    return std::move(E);
  return *Value;
}

BionicResult Bionic::scan(const NativeCallEvent &Call) {
  IntegerScanner Scanner(*this, Call);
  auto Value = Scanner.run();
  if (!Value) {
    auto E = Value.takeError();
    if (!Scanner.isUnsupported())
      return std::move(E);
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = llvm::toString(std::move(E));
    return std::optional<BionicValue>();
  }
  return std::optional<BionicValue>(*Value);
}
} // namespace neverd::emulation::android_model
