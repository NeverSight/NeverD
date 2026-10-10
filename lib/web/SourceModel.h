#pragma once

#include "neverd/web/Source.h"

namespace neverd::web {

// All semantic consumers share the parser-model admission boundary. Returns
// the number of checked edges so each analysis can charge its own work budget.
uint64_t validateSourceModel(const SourceAnalysis &Source);

} // namespace neverd::web
