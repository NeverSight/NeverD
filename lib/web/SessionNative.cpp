#include "SessionInternal.h"

namespace neverd::web {
NativeInput Session::nativeInput(std::string_view Revision,
                                 std::string_view SelectionID) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  auto View = State->artifactView(SelectionID);
  if (!View || View->Origin.getString("kind") == "html_inline_script")
    throw Error("native_selection_unavailable");
  NativeInput Input;
  Input.Content = View->Content;
  Input.BlobHash = View->BlobHash;
  if (Input.Content.size() > MaxNativeInputBytes)
    throw Error("native_input_budget_exceeded");
  Input.Provenance = json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"handoff_id",
       identity("native-handoff", {State->Published.ID, Revision, SelectionID,
                                   Input.BlobHash, NativeHandoffProfile})},
      {"handoff_profile", std::string(NativeHandoffProfile)},
      {"project_id", State->Published.ID},
      {"revision", std::string(Revision)},
      {"selection_id", std::string(SelectionID)},
      {"blob_sha256", Input.BlobHash},
      {"byte_length", std::to_string(Input.Content.size())},
      {"origin", std::move(View->Origin)},
      {"host_path", nullptr},
      {"companion_file_discovery", false},
      {"sidecar_persistence", false},
      {"executes_input", false},
      {"producer_version_verified", false},
      {"redaction_policy", "metadata-only-v1"}});
  return Input;
}
} // namespace neverd::web
