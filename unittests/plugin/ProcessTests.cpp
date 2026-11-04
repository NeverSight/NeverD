//===- ProcessTests.cpp - Process output-budget lifetime ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguish provider totals from bounded persistent-worker deliveries.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Process.h"

#include "neverd/plugin/Config.h"
#include "neverd/plugin/Executor.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace neverd::plugin;

namespace {
constexpr size_t kChunkBytes = limits::kReadChunkBytes / 2;

void check(bool Condition, const char *Message) {
  if (!Condition)
    throw std::runtime_error(Message);
}

int produce(bool Diagnostic, bool Flood) {
  const size_t Limit =
      Diagnostic ? limits::kMaxLogBytes : limits::kMaxTransportBytes;
  auto &Output = Diagnostic ? std::cerr : std::cout;
  const std::string Chunk(kChunkBytes, 'x');
  for (size_t Sent = 0; Sent < Limit + 2 * kChunkBytes; Sent += kChunkBytes) {
    Output.write(Chunk.data(), Chunk.size());
    Output.flush();
    if (!Flood && std::cin.get() != 'a')
      return 2;
  }
  return 0;
}
} // namespace

int main(int Argc, char **Argv) {
  try {
    if (Argc != 2)
      throw std::runtime_error("process budget case required");
    const std::string Mode = Argv[1];
    const bool Diagnostic = Mode.find("diagnostic") != std::string::npos;
    const bool Flood = Mode.find("backlog") != std::string::npos ||
                       Mode.find("discard") != std::string::npos;
    if (Mode.starts_with("child-"))
      return produce(Diagnostic, Flood);
    const bool Lifetime = Mode.find("lifetime") != std::string::npos;
    const size_t Limit =
        Diagnostic ? limits::kMaxLogBytes : limits::kMaxTransportBytes;
    Executor Queue;
    std::unique_ptr<Process> Child;
    size_t Delivered = 0;
    bool Exited = false;
    int ExitCode = -1;
    std::string Error;
    const auto Post = [&](auto Work) { Queue.post(std::move(Work)); };
    const auto Changed = [&](ProcessEvent Event) {
      if (Event.Type == ProcessEvent::Output ||
          Event.Type == ProcessEvent::Diagnostic) {
        check(Event.Type == (Diagnostic ? ProcessEvent::Diagnostic
                                       : ProcessEvent::Output),
              "unexpected output stream");
        Delivered += Event.Bytes.size();
        // The child cannot produce its next chunk until this delivery has
        // been consumed. Total traffic still crosses the lifetime ceiling.
        if (!Flood && Delivered % kChunkBytes == 0)
          check(Child->write("a"), "acknowledging delivered chunk failed");
      } else if (Event.Type == ProcessEvent::Error) {
        Error = Event.Bytes;
      } else {
        Exited = true;
        ExitCode = Event.ExitCode;
      }
    };
    if (Lifetime)
      Child = std::make_unique<Process>(Post, Changed);
    else
      Child = std::make_unique<Process>(Post, Changed,
                                        ProcessOutputBudget::PendingDelivery);
    std::string StartError;
    const auto Program = std::filesystem::absolute(Argv[0]);
    check(Child->start(Program, {"child-" + Mode}, Program.parent_path(),
                       StartError),
          "starting child failed");
    if (Flood) {
      // Retain all completion callbacks. Queued mode must still reject an
      // oversized backlog, without requiring callback progress to join.
      Child->join();
      Child.reset();
      if (Mode.find("discard") != std::string::npos)
        return 0; // Discard charged callbacks after Process destruction.
      Queue.drain();
    } else {
      const auto Deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (!Exited && std::chrono::steady_clock::now() < Deadline)
        Queue.runOne(std::chrono::milliseconds(10));
      const bool Timely = Exited;
      Child->stop();
      Child->join();
      Queue.drain();
      check(Timely, "process output test timed out");
    }
    check(Exited, "missing exit delivery");
    if (Lifetime || Flood) {
      check(!Error.empty(), "output limit was not enforced");
      check(Delivered <= Limit, "excess output reached the consumer");
    } else {
      check(Error.empty() && ExitCode == 0,
            "valid persistent output was stopped");
      check(Delivered == Limit + 2 * kChunkBytes,
            "persistent output was lost or duplicated");
    }
    return 0;
  } catch (const std::exception &Error) {
    std::cerr << Error.what() << '\n';
    return 1;
  }
}
