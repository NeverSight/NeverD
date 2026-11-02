//===- AndroidFormatting.cpp - Bounded guest string formatting ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidArguments.h"

#include <algorithm>
#include <limits>

namespace neverd::emulation::android_model {
// Android 9 LP64 and AAPCS64 policies live together here. In particular, host
// va_list, long, wchar_t, printf and the Windows DbgPrint model are not used.
class Bionic::StringFormatter {
public:
  StringFormatter(Bionic &Model, const NativeCallEvent &Call)
      : Model(Model), Call(Call), Remaining(Model.Options.MemoryLimit),
        CheckedSnprintf(Call.Name == symbol::SnprintfChk ||
                        Call.Name == symbol::VsnprintfChk),
        CheckedSprintf(Call.Name == symbol::SprintfChk ||
                       Call.Name == symbol::VsprintfChk),
        Bounded(CheckedSnprintf || Call.Name == symbol::Snprintf ||
                Call.Name == symbol::Vsnprintf),
        HasVAList(Call.Name == symbol::Vsnprintf ||
                  Call.Name == symbol::Vsprintf ||
                  Call.Name == symbol::VsnprintfChk ||
                  Call.Name == symbol::VsprintfChk),
        FormatArgument(CheckedSnprintf  ? 4
                       : CheckedSprintf ? 3
                       : Bounded        ? 2
                                        : 1),
        Arguments(Model, Call, FormatArgument + 1, Remaining) {}

  llvm::Expected<uint64_t> run();
  bool isUnsupported() const { return Unsupported; }

private:
  Bionic &Model;
  const NativeCallEvent &Call;
  uint64_t Remaining, Capacity = 0, Count = 0;
  std::vector<uint8_t> Output;
  bool Unsupported = false;
  const bool CheckedSnprintf, CheckedSprintf, Bounded, HasVAList;
  const unsigned FormatArgument;
  ArgumentReader Arguments;
  static constexpr uint64_t MaxCount = std::numeric_limits<int32_t>::max();

  llvm::Error unsupported(llvm::Twine Reason) {
    Unsupported = true;
    return failure(Call.Name + ": " + Reason);
  }
  llvm::Error read(uint64_t Address, llvm::MutableArrayRef<uint8_t> Bytes) {
    if (Bytes.size() > Remaining)
      return failure(diagnostic::FormatInputLimit);
    Remaining -= Bytes.size();
    if (auto E = Model.access(Address, Bytes.size(), Read))
      return E;
    return Model.CPU.read(Address, Bytes);
  }
  llvm::Expected<std::string> text(uint64_t Address, uint64_t Bound);
  llvm::Error number(llvm::StringRef &Format, uint64_t &Value);
  llvm::Error emit(uint64_t Size, llvm::StringRef Text, char Repeated = 0);
  llvm::Error render(llvm::StringRef Format);
};

llvm::Expected<std::string> Bionic::StringFormatter::text(uint64_t Address,
                                                          uint64_t Bound) {
  std::string Text;
  // A precision bounds the read, not just the eventual copied prefix. No NUL
  // or readable byte beyond that boundary is required by %.*s.
  for (uint64_t I = 0; I < Bound; ++I) {
    if (Address > UINT64_MAX - I)
      return failure(diagnostic::FormatStringOverflow);
    uint8_t Byte;
    if (auto E = read(Address + I, llvm::MutableArrayRef(&Byte, 1)))
      return std::move(E);
    if (!Byte)
      return Text;
    Text.push_back(Byte);
  }
  return Text;
}

llvm::Error Bionic::StringFormatter::number(llvm::StringRef &Format,
                                            uint64_t &Value) {
  Value = 0;
  while (!Format.empty() && Format.front() >= '0' && Format.front() <= '9') {
    unsigned Digit = Format.front() - '0';
    if (Value > (MaxCount - Digit) / 10)
      return unsupported(diagnostic::FormatWidthPrecision);
    Value = Value * 10 + Digit;
    Format = Format.drop_front();
  }
  return llvm::Error::success();
}

llvm::Error Bionic::StringFormatter::emit(uint64_t Size, llvm::StringRef Text,
                                          char Repeated) {
  if (Size > MaxCount - Count)
    return unsupported(diagnostic::FormatResultLimit);
  Count += Size;
  const uint64_t Keep =
      Capacity ? std::min(Size, Capacity - 1 - Output.size()) : 0;
  if (Keep) {
    // Reserve room for the final NUL. A small destination can still count a
    // very large padding field without allocating or iterating over it.
    if (Output.size() >= Model.Options.MemoryLimit ||
        Keep >= Model.Options.MemoryLimit - Output.size())
      return failure(diagnostic::FormatOutputLimit);
    if (!Model.Budget.remainingMicroseconds()) {
      Model.Expired = true;
      return failure(diagnostic::CallTimeout);
    }
    if (Text.empty())
      Output.insert(Output.end(), Keep, Repeated);
    else
      Output.insert(Output.end(), Text.bytes_begin(),
                    Text.bytes_begin() + Keep);
  }
  return llvm::Error::success();
}

llvm::Error Bionic::StringFormatter::render(llvm::StringRef Format) {
  while (!Format.empty()) {
    size_t Percent = Format.find('%');
    if (Percent == llvm::StringRef::npos)
      return emit(Format.size(), Format);
    if (auto E = emit(Percent, Format.take_front(Percent)))
      return E;
    Format = Format.drop_front(Percent + 1);
    bool Left = false, Plus = false, Space = false, Alternate = false,
         Zero = false;
    while (!Format.empty()) {
      char Flag = Format.front();
      if (Flag == '-')
        Left = true;
      else if (Flag == '+')
        Plus = true;
      else if (Flag == ' ')
        Space = true;
      else if (Flag == '#')
        Alternate = true;
      else if (Flag == '0')
        Zero = true;
      else if (Flag != '\'')
        break; // Android 9 ignores grouping.
      Format = Format.drop_front();
    }
    uint64_t Width = 0, Precision = 0;
    bool HasPrecision = false;
    if (Format.consume_front("*")) {
      auto V = Arguments.next(4);
      if (!V)
        return V.takeError();
      int64_t Signed = static_cast<int32_t>(*V);
      if (Signed == std::numeric_limits<int32_t>::min())
        return unsupported(diagnostic::FormatWidthMagnitude);
      Left |= Signed < 0;
      Width = Signed < 0 ? -Signed : Signed;
    } else if (auto E = number(Format, Width)) {
      return E;
    }
    if (Format.consume_front(".")) {
      HasPrecision = true;
      if (Format.consume_front("*")) {
        auto V = Arguments.next(4);
        if (!V)
          return V.takeError();
        int32_t Signed = static_cast<int32_t>(*V);
        HasPrecision = Signed >= 0;
        Precision = HasPrecision ? Signed : 0;
      } else if (auto E = number(Format, Precision)) {
        return E;
      }
    }
    enum { Default, Byte, Short, Long } Length = Default;
    if (Format.consume_front("hh"))
      Length = Byte;
    else if (Format.consume_front("h"))
      Length = Short;
    else if (Format.consume_front("ll") || Format.consume_front("l") ||
             Format.consume_front("j") || Format.consume_front("z") ||
             Format.consume_front("t") || Format.consume_front("q"))
      Length = Long;
    if (Format.empty())
      return unsupported(diagnostic::FormatIncompleteConversion);
    char Conversion = Format.front();
    Format = Format.drop_front();
    if (Conversion == 'D' || Conversion == 'O' || Conversion == 'U') {
      Length = Long;
      Conversion += 'a' - 'A';
    }
    std::string Digits, Prefix;
    uint64_t Zeros = 0;
    if (Conversion == 's' || Conversion == 'c' || Conversion == '%') {
      if (Length != Default)
        return unsupported(diagnostic::FormatTextLength);
      if (Conversion == '%') {
        Digits = "%";
      } else {
        auto V = Arguments.next(Conversion == 's' ? 8 : 4);
        if (!V)
          return V.takeError();
        if (Conversion == 'c') {
          Digits.push_back(static_cast<uint8_t>(*V));
        } else if (!*V) {
          Digits = "(null)";
          if (HasPrecision && Precision < Digits.size())
            Digits.resize(Precision);
        } else {
          auto S = text(*V, HasPrecision ? Precision : UINT64_MAX);
          if (!S)
            return S.takeError();
          Digits = std::move(*S);
        }
      }
    } else if (Conversion == 'd' || Conversion == 'i' || Conversion == 'u' ||
               Conversion == 'o' || Conversion == 'x' || Conversion == 'X' ||
               Conversion == 'p') {
      const bool Pointer = Conversion == 'p';
      if (Pointer && Length != Default)
        return unsupported(diagnostic::FormatPointerLength);
      unsigned Bits = Pointer || Length == Long ? 64
                      : Length == Byte          ? 8
                      : Length == Short         ? 16
                                                : 32;
      auto V = Arguments.next(Bits == 64 ? 8 : 4);
      if (!V)
        return V.takeError();
      const uint64_t Mask = UINT64_MAX >> (64 - Bits);
      uint64_t Magnitude = *V & Mask;
      if (Conversion == 'd' || Conversion == 'i') {
        if (Magnitude & (uint64_t(1) << (Bits - 1))) {
          Prefix = "-";
          Magnitude = (uint64_t(0) - Magnitude) & Mask;
        } else if (Plus)
          Prefix = "+";
        else if (Space)
          Prefix = " ";
      }
      unsigned Base = Conversion == 'o'                                   ? 8
                      : Pointer || Conversion == 'x' || Conversion == 'X' ? 16
                                                                          : 10;
      if (Pointer || (Base == 16 && Alternate && Magnitude))
        Prefix = Conversion == 'X' ? "0X" : "0x";
      if (Magnitude || !HasPrecision || Precision) {
        const char *Alphabet =
            Conversion == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        do {
          Digits.push_back(Alphabet[Magnitude % Base]);
          Magnitude /= Base;
        } while (Magnitude);
        if (Base == 8 && Alternate && Digits.back() != '0')
          Digits.push_back('0');
        std::reverse(Digits.begin(), Digits.end());
      }
      // API 28 emits no digits even for %#.0o with zero. Its pointer prefix
      // is unconditional, so %.0p with NULL consists of "0x".
      if (HasPrecision) {
        Zero = false;
        Zeros = Precision > Digits.size() ? Precision - Digits.size() : 0;
      }
    } else {
      return unsupported(diagnostic::FormatConversion);
    }
    uint64_t Size = Prefix.size() + Zeros + Digits.size();
    uint64_t Padding = Width > Size ? Width - Size : 0;
    if (!Left && !Zero)
      if (auto E = emit(Padding, {}, ' '))
        return E;
    if (auto E = emit(Prefix.size(), Prefix))
      return E;
    if (!Left && Zero)
      if (auto E = emit(Padding, {}, '0'))
        return E;
    if (auto E = emit(Zeros, {}, '0'))
      return E;
    if (auto E = emit(Digits.size(), Digits))
      return E;
    if (Left)
      if (auto E = emit(Padding, {}, ' '))
        return E;
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> Bionic::StringFormatter::run() {
  const auto &A = Call.Arguments;
  Capacity = CheckedSprintf ? A[2] : Bounded ? A[1] : MaxCount + 1;
  // API 28 ignores flags. The snprintf object guard precedes every guest
  // read, including format and va_list admission; size_t stays full-width.
  if (CheckedSnprintf && Capacity > A[3])
    return unsupported(diagnostic::FormatFortify);
  // Large n has version-specific FILE counter behavior in Android 9. Do not
  // import host behavior, including checked sprintf's SIZE_MAX -> SSIZE_MAX
  // mapping, or promise the overflow/fortify failure path.
  if ((Bounded || CheckedSprintf) && Capacity > MaxCount)
    return unsupported(diagnostic::FormatCapacity);
  auto Format = text(A[FormatArgument], UINT64_MAX);
  if (!Format)
    return Format.takeError();
  if (HasVAList)
    if (auto E = Arguments.initializeVAList(A[FormatArgument + 1]))
      return std::move(E);
  if (auto E = render(*Format))
    return std::move(E);
  // A checked sprintf must fit its final NUL. Stop before publishing the
  // retained prefix: Bionic's partial writes and abort are not modeled.
  if (CheckedSprintf && Count >= Capacity)
    return unsupported(diagnostic::FormatFortify);
  if (Capacity) {
    Output.push_back(0);
    if (auto E = Model.access(A[0], Output.size(), Write))
      return std::move(E);
    if (auto E = Model.CPU.write(A[0], Output))
      return std::move(E);
  }
  return Count;
}

BionicResult Bionic::format(const NativeCallEvent &Call) {
  StringFormatter Formatter(*this, Call);
  auto Value = Formatter.run();
  if (!Value) {
    auto E = Value.takeError();
    if (!Formatter.isUnsupported())
      return std::move(E);
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = llvm::toString(std::move(E));
    return std::optional<BionicValue>();
  }
  return std::optional<BionicValue>(*Value);
}
} // namespace neverd::emulation::android_model
