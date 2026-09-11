// Retain the already-generated input for an explicitly selected CI failure.
#ifndef NEVERD_UNITTESTS_SEMANTIC_FAILURESNAPSHOTINPUT_H
#define NEVERD_UNITTESTS_SEMANTIC_FAILURESNAPSHOTINPUT_H

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace neverd::test {
inline void retainFailureSnapshotInput(std::string_view Name,
                                      const std::string &Source,
                                      const std::string &Object,
                                      const std::string &CompileCommand) noexcept {
  const char *Root = std::getenv("NEVERD_CI_FAILURE_SNAPSHOT_DIR");
  const char *Selected = std::getenv("NEVERD_CI_FAILURE_SNAPSHOT_FUNCTION");
  if (!Root || !*Root || !Selected || Name != Selected)
    return;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
  try {
    namespace fs = std::filesystem;
    std::error_code EC;
    const fs::path Parent(Root);
    if (!fs::is_directory(Parent, EC) || EC)
      return;
    const fs::path Dir = Parent / "fixture";
    // A repeat invocation must not overwrite another invocation's evidence.
    if (!fs::create_directory(Dir, EC) || EC)
      return;
    const auto Deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto Copy = [&](const fs::path &From, const char *To, uintmax_t Limit) {
      EC.clear();
      const auto Size = fs::file_size(From, EC);
      if (EC || Size == 0 || Size > Limit)
        return false;
      std::ifstream In(From, std::ios::binary);
      std::ofstream Out(Dir / To, std::ios::binary | std::ios::out);
      if (!In || !Out)
        return false;
      std::array<char, 16384> Bytes;
      uintmax_t Remaining = Size;
      while (Remaining && std::chrono::steady_clock::now() < Deadline) {
        const size_t N = Remaining < Bytes.size() ? Remaining : Bytes.size();
        In.read(Bytes.data(), static_cast<std::streamsize>(N));
        if (In.gcount() != static_cast<std::streamsize>(N))
          return false;
        Out.write(Bytes.data(), static_cast<std::streamsize>(N));
        if (!Out)
          return false;
        Remaining -= N;
      }
      // Confirm the retained stream is exactly the previously measured file.
      const bool Exact = Remaining == 0 && In.peek() == std::char_traits<char>::eof();
      Out.close();
      return Exact && !Out.fail();
    };
    auto Text = [&](const char *File, std::string_view Value) {
      if (Value.size() > 65536)
        return false;
      std::ofstream Out(Dir / File, std::ios::binary | std::ios::out);
      Out.write(Value.data(), static_cast<std::streamsize>(Value.size()));
      Out.close();
      return !Out.fail();
    };
    const bool SourceWritten = Copy(Source, "source.c", 1024 * 1024);
    const bool ObjectWritten = Copy(Object, "input.o", 16 * 1024 * 1024);
    const bool NameWritten = Text("function.txt", Name);
    const bool CommandWritten = Text("compile-command.txt", CompileCommand);
    std::ofstream Receipt(Dir / "retention.json", std::ios::binary | std::ios::out);
    Receipt << "{\"schema\":1,\"compile_succeeded\":true"
            << ",\"source_written\":" << (SourceWritten ? "true" : "false")
            << ",\"object_written\":" << (ObjectWritten ? "true" : "false")
            << ",\"function_written\":" << (NameWritten ? "true" : "false")
            << ",\"command_written\":" << (CommandWritten ? "true" : "false")
            << "}\n";
    Receipt.close();
  } catch (...) {
    // The existing test retains its original compile/lift/emulation result.
  }
#else
  (void)Source;
  (void)Object;
  (void)CompileCommand;
#endif
}
} // namespace neverd::test
#endif // NEVERD_UNITTESTS_SEMANTIC_FAILURESNAPSHOTINPUT_H
