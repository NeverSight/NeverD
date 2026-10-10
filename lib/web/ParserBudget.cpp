#include "neverd/web/ParserBudget.h"

#include "neverd/web/Session.h"

namespace neverd::web {
namespace {
thread_local ParserBudget *Active = nullptr;
}

ParserBudget::ParserBudget()
    : Previous(Active),
      Deadline(std::chrono::steady_clock::now() + std::chrono::seconds(5)) {
  Active = this;
}

ParserBudget::~ParserBudget() { Active = Previous; }

void ParserBudget::charge(size_t Count) {
  constexpr uint64_t MaxBytes = 32 * 1024 * 1024;
  if (Count > MaxBytes - Bytes || ++Work > 200000)
    throw Error("parser_budget_exceeded");
  Bytes += Count;
  if ((Work & 63) == 0 && std::chrono::steady_clock::now() > Deadline)
    throw Error("parser_deadline_exceeded");
}

void chargeParserAllocation(size_t Bytes) {
  if (!Active)
    throw Error("parser_budget_missing");
  Active->charge(Bytes);
}
} // namespace neverd::web
