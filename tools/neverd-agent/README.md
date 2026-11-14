# NeverD Agent

`neverd-agent` is a standalone C++20 / Qt Quick desktop application. The Windows
product is `neverd-agent.exe`; macOS uses an application bundle and Linux an
executable. All application implementation is private to this directory.

The desktop workspace connects to model APIs, delegates project questions to
independent object analysts, and runs a model/tool loop through NPH and ND's
read-only MCP engine. Object analysts can create their own independent subtasks.
The UI presents projects, analysis objects, conversations and progress; internal
Router/Worker/Subagent roles are not exposed as navigation concepts.

## Build and run

The normal desktop distribution installs Agent with ND GUI, CLI and analysis
helpers in the same product directory. On Windows/Linux, the executables share
`<prefix>/bin` and one Qt runtime. On macOS, the two applications are separate
`.app` bundles under the same installation prefix; Agent discovers the adjacent
ND bundle's helpers. Source separation under `tools/neverd-agent` does not
require a separate installed product.

With the GUI enabled, build `neverd-desktop` and install that build once. See the
[desktop packaging recipe](../neverd-gui/PACKAGING.md). This builds the enabled
applications, worker, MCP and plugin-host CLI together. The standalone build
below remains available for work that only changes Agent.

Requires Qt 6.8+ Core, Gui, Quick, QuickControls2, Svg, Concurrent and Network, a C++20
compiler, and CMake 3.24+. Tests additionally require Qt Test. The application
can be built independently of LLVM and the engine. Use a developer shell on
Windows and set `CMAKE_PREFIX_PATH` to the desired Qt installation.

```sh
cmake -S tools/neverd-agent -B build-agent -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
python scripts/build.py build-agent --target neverd-agent NeverDAgentTests NeverDAgentModelTests NeverDAgentMarkdownTests NeverDAgentInteractionTests NeverDAgentMemoryTests --parallel 4
ctest --test-dir build-agent -R '^NeverDAgent\.' --output-on-failure
cmake --install build-agent --prefix agent-release
```

On Windows, also build `NeverDAgentFrameTests` to run native resize-hit and
maximize/restore checks. Unix standalone builds need Qt Gui private development
headers for the workbench's existing Wayland frame adapter. Linux also requires
pkg-config, libsecret development files (`libsecret-1-dev` on Debian/Ubuntu) and
OpenSSL Crypto development files (`libssl-dev`, used by NPH). Its desktop session
must provide an unlocked Secret Service, such as GNOME Keyring or a compatible
KDE service. macOS uses the SDK's Security and CoreFoundation frameworks.

The repository's build entry point refreshes its dependency snapshots even for
this standalone target; no engine library is linked into this milestone.
`NEVERD_BUILD_AGENT` defaults to ON for desktop builds and can be set to OFF
for workbench-only builds. Its standalone source entry point always builds
Agent. All included components use the build's shared runtime output and
installation prefix.

Qt deployment is part of the install step. Distribute the installed directory,
including Qt libraries, plugins and QML imports, rather than copying the exe
alone. The application uses the existing NeverD logo and its own consistently
stroked, monochrome Qt Quick vector action glyphs (`ui/Icon.qml`). No external
agent source code or runtime is embedded.

The Agent shell shares ND GUI's frame metrics, window controls and look. Every
color in `ui/Theme.qml` is a role of the workbench's `ChromeColors.def` (Visual
Studio Code Dark+ and Light+), read through `AgentWindowFrame::chromeColor`:
the title band, panes a step apart parted by hairlines, the selection blue
under white text, fields, check boxes, menus and one main button in the accent
per window. Corners follow the workbench (fields and push buttons 2, tool
buttons and menu highlights 3, cards and sheets 6, panes none), and the window
frame is the workbench's square edge and fading shadow. There are no gradients,
blur or translucent panels.
The product logo appears only at the top-left of the caption; conversation
messages and empty states use text hierarchy without repeated brand avatars.
Project/object navigation and the initially hidden right pane keep their existing
behavior. Caption-level controls and keyboard focus remain visible and reachable.
Action glyphs share an 18-pixel optical size and 34-pixel hit target, including
the send/stop and disclosure controls. Spacing follows 8/16/24-pixel steps.
Text uses the bundled Noto Sans and Noto Sans CJK at the desktop's interface
size, as the workbench does; secondary text is quieted by color, not size.
Markdown receives the actual view font explicitly instead of retaining a
separate document default. JetBrains Mono is limited to code.

Compaction behavior was compared with the local Codex source at revision
`95e1d5993` (`core/src/compact.rs`): exact earlier user messages and a checkpoint
can coexist in the replacement context. Agent additionally preserves complete
recent native tool exchanges and excludes that retained suffix from summary
input. This is an independent implementation for the three supported protocols.
OpenAI's [native compaction](https://developers.openai.com/api/docs/guides/compaction)
also supports opaque server-produced items; choosing a Responses-compatible URL
does not establish that the provider implements that endpoint, so this version
does not silently invoke it on a custom provider.

For native layout checks, `--capture output.png --capture-size 1024x768` controls
logical window size without changing saved user settings. Captures wait for text
layout and effect textures to settle; this delay does not affect normal startup.

```sh
neverd-agent --data-dir /path/to/agent-data
neverd-agent --project /path/to/research
```

`--data-dir` overrides the default `<user home>/.nd_agent` and supports portable
installations. `--show-panel files|progress|memory|chat` opens a sidebar view. The
optional `--capture <png>` takes a native Qt capture and exits, for isolated UI
verification. Use a disposable data directory for testing.
`--show-settings` opens model configuration directly.

## Chat in NeverD GUI

The desktop installation supplies and enables the **NeverD Agent** native GUI
plugin on first use. Its enabled setting is managed in
**Plugins → Plugin manager…**. The floating ball appears at the bottom-right
with the chat closed. Hover over it to preview or click it to focus the chat.
Choose **Pin** to keep it open when the pointer leaves. Drag the ball to move
it inside the workbench. A pinned panel follows it and opens on the side with
room. Press Escape or click the ball again to close the chat.

Right-click the ball and choose **Hide floating ball**. The first hide shows
where to restore it. **Plugins → NeverD Agent → Show floating ball** toggles
the launcher, and its visibility is remembered across launches. Hiding it
keeps the conversation and active request. Use **Stop** to cancel an answer.

Open an analysis object and send a question with **Enter**. **Shift+Enter**
inserts a new line. **Attach current selection** adds
the captured function, address and selection to the input. The plugin uses the
model settings and system credentials configured in the standalone Agent.
Use **Settings** in the panel to open that configuration. Opening the panel
starts `neverd-agent-cli serve --stdio` to read saved messages. Saved
execution records and evidence are available in this read-only view. Sending a
message loads the model settings and connects the analysis tools to the current
workbench session through `neverd-mcp --attach`. Clicking an address evidence
link navigates in the current workbench.

While a response is running, its bubble shows the latest tool action. Expand
**Execution** to see the recorded tool names, analysis objects, addresses,
completion states and elapsed times. Activate a tool row with evidence to
navigate to its address.

Hiding the panel keeps the conversation and active request. Switching projects
or disabling the plugin closes that project's connection and cancels its run.
Workbench conversations are stored per project/input under
`~/.nd_agent/workbench`. Set `NEVERD_AGENT_HOME` before launching the workbench
and standalone Agent to use a different shared data location. The workbench
panel has read access to the active analysis and can provide written suggestions
for engineering changes.

## Agent CLI

`neverd-agent-cli` owns the Agent runtime, project locks, model settings,
credentials and conversation persistence. Both desktop frontends start it as a
child process and use its versioned JSON protocol over standard streams. The
standalone window requests a project workspace, and the workbench plugin binds
the current ND session. The executables can use the same installed CLI and
Agent data directory.

Run a question in an existing Agent project using the configured model profile.
An optional `--object ID` restricts the analysis scope.

```sh
neverd-agent-cli run --project /path/to/research --prompt "Summarize the entry points"
```

`serve --stdio` reads and writes newline-delimited UTF-8 JSON objects. A client
starts with `{"command":"initialize","protocol":1,"mode":"workspace"}` or
`mode:"workbench"` with a captured `context` object. Workspace clients may
include a `project` path. The CLI publishes a versioned `ready` event and
session state. Both modes use `invoke` with a positive `id`, a `method`,
and an `arguments` array. Workspace methods are in
`client/WorkspaceProtocol.def`. Workbench methods include `prepare` with an
attachment credential, `send` with text and the captured context, and `stop`.
Responses carry the request ID and a result or error. Workspace state,
transcript updates and workbench tool execution arrive as events. Keep stdin
open while receiving events. Closing it cancels active work and closes the
session.

A workspace client that presents approval requests can set
`interactiveObjectApprovals: true` in `initialize`. It receives pending
requests in `analysisObjectApprovals`, scoped by `projectId` and `sideChatId`,
and calls `resolveAnalysisObjectApproval`
with the request ID and a boolean decision. The terminal `run` command reports
that interactive approval is unavailable when a new object requires it.

## First use

1. Create a named project in a chosen directory, or open a directory containing
   `.nd-agent/project.json`. A project directory holds notes, scripts and reports.
2. Right-click the project name to add analysis objects. Files remain in their
   original locations. SHA-256 calculation runs outside the UI thread.
3. Open settings (`Ctrl+,`) → **模型与分析设置**. Select a protocol, API URL,
   model ID, API Key, context/output limits and `neverd-mcp` executable. The
   **测试连接** button sends a short model request without project data. Save.
4. Enter a project-wide or object-specific request and press **Enter** or
   choose **发送**. Use **Shift+Enter** for a new line in either the project
   conversation or side chat. The model can use registered tools, delegate
   analysis, and write reports. **停止** cancels the project's current task tree.
5. Use the top-right sidebar button, or a project/object context menu, to open
   files, progress or object memory. The sidebar starts hidden. File views
   always follow the active project; there is no second project selector.
   The speech-bubble button opens **侧边聊天**; the **目标** button above the
   main conversation opens persistent objective execution.
6. To use **Open in ND GUI**, configure the workbench executable in settings.
   Otherwise the app checks its own executable directory and `PATH`. macOS
   settings also accept an `.app` bundle. GUI, MCP and write-worker discovery
   share the same installation resolver, including `Contents/MacOS`, adjacent
   release/build bundles and the CMake installation's `bin` directory. An explicit
   MCP executable takes precedence; its matching write worker must be beside it.
   Executable settings also accept local file URLs or an installation directory.
   Launch uses explicit argument arrays.

Project and object menus are available through right click, the visible `···`
button, and the menu key. `Ctrl+N` creates a project, `Ctrl+O` opens one,
`Ctrl+,` opens settings, and `Ctrl+B` toggles the right sidebar. Shortcut hints
use Qt's native display format, including Command symbols on macOS.

File and folder selection uses the operating system's native dialogs where
available, including project directories, analysis objects, executable selection
and memory import/export. Qt supplies its standard fallback on platforms without
native dialog support.

## Storage and ownership

```text
<user home>/.nd_agent/          # global application directory
  config.json                 # projects, appearance, model profile, key reference
  credentials/<uuid>.key      # Windows DPAPI data; legacy Unix files migrate
  catalog.json                # rebuildable object fingerprint index
  agent.log

<project>/.nd-agent/           # project-local authoritative state
  project.json                # project ID, objects, file references, hashes
  router/conversation.json    # project dialogue, status and current draft
  router/context.json         # completed provider-native project turns
  router/memory.json          # durable project decisions/preferences/procedures
  router/goal.json            # current objective, limits, usage and progress
  goals/<goal-id>.json        # previous goals archived when replaced
  side-chats.json             # side conversation IDs and active selection
  sessions/<session-id>/
    router/conversation.json  # inherited messages, side replies and side draft
    router/context.json       # forked checkpoint and independent model history
    objects/<object-id>/context.json # object context private to this side chat
  objects/<object-id>/
    memory.json               # this project's memory for this object
    context.json              # completed provider-native object turns
  runs/<run-id>/events.jsonl   # model-visible requests, tool results and receipts
  memory/sources/<agent-id>.json # redacted completed exchange and extraction status

<project>/reports/            # new Markdown artifacts, unique filenames
```

Project IDs and object IDs are UUIDs independent of runtime ownership tokens.
An object receives a separate local ID in every project, even when its file hash
is identical. Paths inside a project are stored relatively so moving the whole
project preserves those references; external paths remain absolute. Missing
external files are not silently rebound to another file with the same name.
Foreign absolute paths remain intact on reopening and saving a project on
another OS. Right-click an object and choose **重新定位文件…** to select its new
location. A background SHA-256 check must match the original object; a different
binary/version is refused. A successful rebind preserves the object ID, memory,
conversations and evidence references, retires cached analysis processes, and
updates side chats. Rebinding is refused while that project is analyzing or
applying changes, including if work starts while the file is being hashed.

State uses versioned UTF-8 JSON and `QSaveFile` replacement. Malformed state is
reported without resetting it. A data-directory lock and retained project locks
prevent two desktop instances from overwriting the same loaded project. These
locks do not turn all project updates into a multi-file transaction.

Memory keeps the existing version-1 manual records and adds optional structured
automatic records. Each generated entry has a stable topic key, category,
original quote, run/session identity and captured evidence identities. Project
memory lives in `router/memory.json`; object memory remains bound to the exact
SHA-256 in `objects/<id>/memory.json`. Export/import includes automatic records
and their portable attribution, without exporting the source conversation or
source evidence files. Opening imported evidence requires its source project;
the original quote and IDs remain readable offline. Import checks the exact file
hash, deduplicates identical IDs and refuses changed contents under the same ID
without changing destination memory. Different-version import is unsupported.

### Long-term memory

**应用设置** has separate **分析时召回已有记忆** and
**空闲时自动整理长期记忆** controls. Both start enabled for this local Agent;
global sharing remains off. Generation uses the configured model/protocol/key
and incurs separate API usage. Disabling generation keeps existing notes and
pending local sources; disabling use affects subsequent runs, including their
memory tools and initial index. It does not erase content the model already saw
in an existing conversation. Conversation compaction is independent of memory.

Successful project and object tasks save a redacted completed exchange; failed
or cancelled tasks do not contribute. Subtasks contribute through the object
task's final answer and captured evidence, rather than each creating duplicate
extractions. Side chats contribute only their new exchange, not inherited
history. After 15 seconds without foreground analysis, a cancellable background
request proposes reusable notes. A new foreground request cancels maintenance
and retains its source for a later idle period. Pending work also resumes on
reopening the project. Provider/validation failures retain a failed source and
do not automatically retry it; **立即整理** explicitly retries failed work.

Extraction may return no notes. It separates preferences, research procedures,
binary findings and hypotheses. Preferences must quote the user request;
findings must reference successful, complete ND evidence captured in that run.
Quote/evidence checks establish attribution, not truth: a model can still draw
an incorrect inference from real evidence. All automatic discoveries remain
historical leads requiring current ND verification. Exact duplicates are
ignored; different text under the same topic key is marked **冲突待确认** and
excluded from automatic recall until the user edits/confirms a version. The
other versions remain visible as superseded notes.

At task start, deterministic lexical/API/CJK matching injects a small relevant
index, rather than entire conversation histories. `search_memory` retrieves
matching records and `read_memory` returns a selected note with its attribution.
Project decisions are available within that project; object notes are restricted
by object identity, with optional same-hash shared notes. This first version
does not use embeddings, semantic similarity deduplication or automatic proof
of contradictions under different topic keys.

Open the **记忆** sidebar to inspect project/object notes, view source quotes and
ND evidence, add/edit/confirm or forget notes, import/export object notes and
run maintenance immediately. Forgetting removes the record and stores local
topic/text exclusion keys, preventing an in-flight extraction or later import
from silently reinstating that same note. Existing raw run logs and completed
source packets are diagnostic history, not deleted by forgetting a note.
Credentials are redacted before completed exchanges enter memory storage; never
intentionally put secrets into notes or exports. Review an export before sharing.

The background-extraction and independent use/generation controls follow the
documented [Codex memory behavior](https://learn.chatgpt.com/docs/customization/memories).
This is an independent implementation using the existing NeverD model client,
project persistence and evidence store; it does not read Codex private memories
or copy a Codex runtime/storage format.

**Global sharing** defaults off. Enabling it builds an index of objects in
successfully bound projects and displays matching memories from other projects
read-only. The source file is read when the view is refreshed; the index never
substitutes for the source memory. Disabling sharing hides those references and
empties the derived index; imported local records remain. Project conversations
are never included. This milestone uses JSON for the small, rebuildable catalog;
there is no full-disk scan or remote synchronization.

## Adding dependencies during analysis

When a task needs a local dependency, the Agent can call `add_analysis_object`
with its path, source object and reason. A filename resolves next to the source
binary, or under the project directory if there is no source object. For a file
elsewhere, provide its full local path. The tool returns the registered object
ID so the same task can use the ordinary ND analysis tools on that dependency.

In **应用设置 → AI 添加分析对象**, **逐次审批** is the default. The request
appears just above the initiating conversation's composer with the resolved file
path, destination project, dependency source and reason. While the request
waits, users can edit drafts, queue messages, browse history and switch projects.
Choosing **允许一次** registers the file in that project. Declining resumes
the task with the existing objects. **自动允许** is an explicit application-wide
setting that approves Agent-requested additions automatically. Files stay in their
original locations and are opened for read-only analysis. Registration records
the file hash and the execution record shows the tool outcome.

The project workspace provides this action to the main and side chats. It applies
to the standalone Agent's projects, including analyses started from its CLI.

## Model configuration and execution

Supported protocols are **OpenAI Responses**, **Anthropic Messages** and
**OpenAI-compatible Chat Completions**. API roots ending in `/v1` and complete
endpoint paths are accepted; other roots get `/v1/<protocol path>` appended.
For providers with a different root convention, enter the complete endpoint.
Chat's output field is selectable (`max_completion_tokens` or `max_tokens`), and
the Anthropic version header is configurable. No automatic model substitution,
HTTP redirect with credentials, or tool-effect retry is performed.
Optional reasoning effort maps to Responses `reasoning.effort`, Chat
`reasoning_effort`, or Messages `output_config.effort`. Empty leaves the server
default unchanged.

Windows keys are encrypted with current-user DPAPI. macOS uses
[Keychain generic-password items](https://developer.apple.com/documentation/security/adding-a-password-to-the-keychain);
Linux uses [libsecret / Secret Service](https://gnome.pages.gitlab.gnome.org/libsecret/libsecret-c-examples.html).
New Unix credentials are never written to plaintext files. On first read, an old
Unix credential file is migrated under the same UUID and removed only after the
native keyring write succeeds. A locked/unavailable service produces an explicit
error; it does not select a weaker storage method. Credentials are user/machine
local and are not part of portable project or memory exports.

Keyring I/O runs off the GUI thread. Startup loads the configured key into the
controller's private session cache; each analysis captures that value. If a key
is changed outside the app or the keyring needs unlocking, use **重新读取系统凭据**
in model settings. The settings UI never reads the stored secret into a text
field. Saving a replacement first stores the new secret, commits the configuration
reference, then removes the old credential. A failed settings commit keeps the
previous profile and cleans up the new secret. The dialog closes only after a
successful commit. Close waits for pending credential operations; cancel an OS
unlock prompt if you do not want to complete it. Secrets are absent from project
data, model message bodies and run logs.

Each run captures its model configuration and key. Changes apply to subsequent
runs. Each project, object and subtask accounts for its own current context.
The meter displays the last provider input plus output, including cached input;
it is not cumulative billing. New messages/tool results use a byte-based delta
estimate calibrated to that measurement, explicitly labelled approximate.
Missing usage and a newly compacted context use an uncalibrated estimate.
This is not an exact provider tokenizer. Output space is reserved separately.

At 80% of the remaining input allowance, the loop creates a semantic checkpoint
using the configured model and protocol, with no tools exposed to that request.
This works with Responses, Messages and Chat Completions without requiring a
provider-specific compaction endpoint. It runs before a model step, after the
entire preceding tool batch (including delegated work) has completed. The exact
current question, canonical system/object identity, and a budgeted suffix of
recent complete exchanges remain in the next input. This can retain several
recent turns or in-progress tool batches, rather than only the final exchange.
The retention budget reserves space for the summary output. A separate portion
keeps recent earlier user requests verbatim, marked as historical requests;
the current request and its accepted corrections remain exact. Only the older
prefix and prior checkpoint are sent to the summarizer. Retained native tool
evidence is not summarized again. The checkpoint
preserves objectives, constraints, object IDs/hashes, evidenced facts/addresses,
report paths, failed attempts and unfinished work. Opaque reasoning is omitted
from summary evidence; retained native exchanges keep their signed blocks.
Actual edit receipts, pending proposals and uncertain writes stay distinct in
the checkpoint. Compaction statistics record the retained turns, tool batches,
user requests and summary-request count only after successful installation.

Large saved histories are summarized in bounded text segments; native tool-call
and result pairs are never split. Intermediate summaries are private to the
compaction, not streamed as an answer. Only a complete, useful checkpoint is
atomically persisted and installed. Failure/cancellation retains the prior
checkpoint and evidence. A request/system/tool schema that alone cannot fit,
or a non-reducing summary, fails clearly instead of repeatedly compacting or
silently dropping history. Summary calls contribute to request statistics and
provider usage. Defaults live in `models/ModelProfile.h`.

Context format v2 saves checkpoints and usage next to native groups; v1 histories
remain readable. Original conversation and run JSONL records are not compacted.
The audit includes summary requests/responses and before/after estimates, so the
checkpoint can be compared with its source evidence. Reopening restores it;
changing providers retains visible checkpoints but resets usage calibration.

The composer ring shows **current context tokens / compaction threshold**, not
cumulative billed usage. Hover for occupied tokens and the remaining distance
to compression; click pins the popup. The threshold is 80% of the configured
context window after reserving output tokens. Provider measurements calibrate
local estimates; estimated counts are labelled. Compression resets the current
occupancy; it does not reset cumulative goal usage. Main and side conversations
have separate rings.

## Investigation interactions

The right sidebar starts hidden. Its view menu opens **研究记录**, **函数证据**
and **变更审阅**, in addition to files, object progress, memory and side chat.

- **Execution records:** expand a reply's execution row to inspect actual tool
  requests, results, object/delegation identity, duration and skipped calls.
  Completed records are read from the saved run JSONL after restarting.
- **Follow-ups:** sending while busy queues a captured request for the next
  turn. The adjacent menu can instead supplement the current task. An active
  tool finishes before applying that direction; announced but unstarted calls
  receive explicit skipped results so provider histories retain valid pairing.
  Accepted corrections are retained through compression. Switching object scope
  requires a queued turn. Stop pauses dispatch; reopening never sends pending
  requests automatically. Main and side drafts/queues remain independent.
- **Context references:** use **+** or `@` to attach a registered object or
  function/address. File previews, evidence and selected text can be captured
  too. Chips show the exact captured data and can be removed. Enqueue freezes
  those references; later draft edits cannot change them. Dispatch rejects
  removed or changed object identities. Selected text can also seed a new side
  conversation with the main-history snapshot.
- **Evidence:** ND queries persist object/hash/run/query/revision identity and
  their text under `.nd-agent/evidence/<id>/`. Large process-local MCP snapshot
  URIs are paged into local files while the producing process is alive. Partial
  capture remains explicitly partial. Historical evidence never resolves an old
  resource serial against a replacement process. Open evidence links in answers
  or execution records; the inspector can request C, disassembly, xrefs or CFG
  directly. These are ND's results, not model-generated source. CFG currently
  uses the native textual result. **在 ND GUI 打开** opens the corresponding
  object; the current GUI launch protocol does not expose an address jump.
- **Research steps:** `read_research_plan` / `update_research_plan` maintain
  per-conversation steps, outcomes, evidence links and unresolved questions.
  The main goal is displayed separately. Completed steps without ND evidence
  are labelled accordingly; a model-reported step is not a proof. Steps alone
  never launch another request. Side plans have their own session directory.

### Reviewed ND changes

`propose_change` generates a persisted review item for a symbol name, address
comment, function declaration or local C types. It captures the actual ND
before-value, proposed value, rationale and optional saved evidence IDs. Type
proposals also list affected `type_names`; only these declarations are shown
in the review. The full rendered library's hash is retained for drift checking,
without copying the system-header library into each review item. Proposing
does **not** change the ND project.

**应用变更** is an explicit user action. It requires idle analysis, side chats
and inspector jobs for every open Agent project referencing that source path.
Cached read-only engines are retired before starting a separate writer. The
client rechecks the binary fingerprint and before-value, uses the existing ND
worker revision/writer lock, saves staged changes, and records the engine's
history receipt. `neverd-worker` is resolved beside the configured MCP or GUI,
beside Agent, then through `PATH`; it must have its matching engine library.
If a GUI holds the writable project lock, close that analysis session first.
This version does not attach to a live writable GUI session.

**撤销此变更** requires the current history head, cursor, total and source hash
to match the recorded receipt exactly. It never searches backwards for a similar
entry or undoes an intervening command. ND history currently has no persistent
command UUID; this is a state-matching undo contract, not proof of command
identity after an externally reconstructed identical history. A conflicting
undo keeps the item's applied status and records the reason. Use ND's own
history UI for more complex histories.

Write intent is persisted before submitting a mutation. Lost acknowledgements
remain `uncertain`; a confirmed save without a history receipt remains
`applied_unverified`. Neither is automatically retried or reported as unchanged.
Applying and undoing comments both include a durable save. Worker exit releases
the writer before the UI reports completion. The original input binary is not
patched; these commands modify ND project metadata and type declarations.

## Side conversations

Opening a new side conversation captures the main conversation at that moment.
It inherits the existing checkpoint, completed model/tool exchanges and visible
history. A main turn still in progress is captured as public evidence: completed
tool results remain available, pending calls are labelled pending, and unmatched
native calls are not replayed. Creating a side conversation sends no model request.
Inherited messages are initially collapsed under **已继承主对话** and can be
expanded for inspection.

After that fork, each side conversation has its own draft, transcript, context,
compaction and runtime. Later main messages do not silently update an existing
side conversation; use **+** to branch from the latest main context. Main and side
requests can run at the same time. The history selector restores earlier side
conversations, including after restarting the app. **引用** explicitly copies an
answer into the main draft without sending it or changing the main model history.

Side conversations share their project's registered analysis objects, report
folder and long-term object memories. Their model contexts and usage are separate;
they do not inherit running tool handles or the main goal scheduler. Run records
include the session ID, and the copied transcript records its fork timestamp.

## Persistent goals

Use **目标** to enter an objective, cumulative token budget and maximum number of
rounds. Each round uses the existing project analysis runtime and main context.
Before the final answer, the model reports progress through `report_goal_progress`:
`continue` requires a concrete next step, `completed` requires evidence for the
whole objective, and `blocked` records why progress needs attention. A successful
`continue` round schedules the next one automatically. A final answer alone does
not mark a goal complete or create an unbounded retry loop.

**暂停** cancels the current task tree and prevents another round. Manual main
requests also stop automatic continuation. Completion, a blocked/missing progress
report, a model/tool failure, or a token/round limit stops execution with a visible
status. Editing/resuming preserves accumulated usage and round counts. Closing
the app preserves progress; interrupted goals always require explicit resume on
reopen. No background scheduler is installed.

The goal's usage is cumulative input plus output across the main task, delegated
tasks and compaction requests. It differs from the ring's current-context size.
Before a request, the runtime reserves estimated input plus permitted output
from the shared goal budget. It settles with provider usage when available.
Missing usage is labelled estimated; interrupted requests conservatively retain
their reservations rather than resetting the allowance. Actual tokenizer counts
can differ from the admission estimate, so this is not an exact billing cap.
Persisted pending reservations are also charged conservatively after interruption.
Goal completion is an evidenced model progress report, not automatic proof of
binary-analysis correctness; the recorded findings and tool receipts remain
available for review.

## Execution and analysis boundaries

Failed/cancelled turns preserve the question, completed tool results and visible
failure summary. Announced calls without results receive explicit interruption
results; these do not establish whether a tool effect completed and must not be
replayed automatically. An explicit continuation can reuse the evidence already
obtained. Invalid saved context is reported without replacing it.
Changing provider identity uses previous visible question/answer pairs rather
than forwarding opaque continuation data to another provider.

The project conversation, object analysts and subtasks have no model-call count
limit. Request counts are progress statistics, including compaction requests;
they never disable tools, force a final answer or stop a task. Obsolete
`maxSteps` settings in older configuration files are ignored and are omitted
when settings are next saved. Context compaction and user cancellation remain
available. Goal token usage and reservations remain shared across all contexts.

Ordinary tool calls execute in order. Registrations explicitly marked concurrent
can overlap within a tool batch; their results still enter model history in the
original call order. Object delegations use this path, start within the configured
live-task limit (including tasks admitted but not started), and queue excess
objects. A subtask requested when no slot is available returns a diagnosis asking
its parent to perform that work directly, avoiding a parent/child scheduling
deadlock. Nesting depth also remains bounded. Separate projects can run concurrently.
The coordinator answers planning questions with a project-specific research route
before expanding into broad object analysis. During an actual investigation, the
conversation shows per-object progress without requiring the raw tool log to open.
Cancellation stops model requests and propagates through NPH to child tasks.
An ND query cancellation stops waiting for its result; the engine may finish an
already-running computation. Interrupted requests are marked on reopen, never
silently retried. Subtask evidence remains in run logs; completed object and
project contexts survive subsequent requests.

ND analysis uses one read-only `neverd-mcp --file` process per object in a
project runtime. The executable must ship with its matching engine library.
It is found from the configured path, Agent's executable directory, configured
ND GUI directory, or `PATH`. Object identity is verified before process start.
The agent discovers tool schemas, invokes read-only calls, and can read paged
`neverd://` resource results. It does not expose arbitrary shell commands.
Project file reads are scoped to ordinary UTF-8 project files, outside private
state; reports create new files. Shared object memory follows the opt-in hash
index. Private conversations are not shared between projects.

## Implementation boundaries

- `AgentCli.cpp` owns the command entry point, shared protocol handshake and
  standard-stream transport. `bridge/` binds the workspace and workbench
  session modes to the runtime. `client/WorkspaceClient.*` exposes the CLI's
  workspace state to QML, and both frontends use `client/AgentProcess.*`.
- `project/ProjectStore.*` owns persistent schemas, validation, binary identity
  and memory exchange. It has no QML or model-provider dependency.
  `GoalState.*` owns persisted objective validation and interrupted-run recovery.
- `platform/AppPaths.*` resolves user directories and executables.
  `AgentWindowFrame.*` adapts Qt Quick input to the same QWindow-only platform
  frame sources used by the ND workbench. Caption metrics and chrome colors
  also come from the workbench's definitions; no GUI widgets or analysis
  services are linked. No platform shell commands or developer-specific paths
  are used.
- `models/` owns endpoint/profile validation, credentials, streaming codecs and
  asynchronous HTTP. Only complete successful model turns dispatch tools.
- `runtime/NphTools.*` consumes the existing `neverd_tools` library and captures
  exact tool registrations per model step. NPH retains validation, owner
  activation, cancellation, retirement and receipts.
  Text limits are published in tool schemas; report/delegation parameters use
  an explicit 65,536-character limit instead of the legacy short-field default.
  ND query arguments are native JSON objects, validated as objects by NPH and
  against the selected tool's schema by ND. Results use explicitly open object
  schemas because each discovered ND service owns its nested payload contract.
- `runtime/AgentLoop.*` owns one model context and complete-turn persistence.
  `ResearchRuntime.*` owns project delegation, object sessions, subtasks and
  run evidence. `McpSession.*` is the asynchronous object-bound ND transport.
- `app/AgentController.*` and `AgentModelController.*` bind exact project and
  object identities to actions and asynchronous fingerprinting. Switching
  projects flushes the old draft before changing context.
  `SideChat.*` and `AgentSideChatController.*` own forked conversations;
  `AgentGoalController.*` owns goal transitions and round scheduling. Both reuse
  the same runtime, provider support and NPH tools.
- `ui/` contains the project sidebar, conversation and optional tool views.
  It paints its own caption, minimize/maximize/close controls, frame and shadow.
  The window manager still performs native movement, resizing and window-state
  transitions. Edge dragging from a maximized window restores it for resizing,
  matching the workbench. Workbench color roles and the bundled Noto fonts
  provide normal text; JetBrains Mono is reserved for code.
  OS-level desktop acrylic is not enabled. Font license notices are included
  in installed products.

Model selection per object, automatic semantic consolidation across different
topic keys, and attaching to an existing GUI analysis session remain future
work. Analysis queries are read-only; ND edits follow the existing change-review
flow. No engine, IR pass or analysis semantics are reimplemented here.
The current Agent registers its workspace actions and ND MCP adapter; it does
not yet discover external process-plugin catalogs automatically.

Conversation messages use left/right neutral gray bubbles, selectable native Qt
Markdown and copy controls. Headings, lists, emphasis, links, fenced/inline code
and tables use Qt's GitHub Markdown parser. Right-click offers selection, code
block and original Markdown copying. Streaming updates are coalesced within the
changed message; scrolling back does not intentionally follow the tail. `.md`
and `.markdown` project files use the same renderer; other text stays plain.
Raw HTML is treated as text; images appear as labels without automatic resource
loads. Clicked web links open the browser; existing text-file links within the
current project open in the preview. Code is selectable/wrapped, without syntax
highlighting in this version.

The current executable opens the original file in ND GUI; it does not attach
the Agent's conversation or memory to an existing GUI analysis session. The
file pane is a manual-refresh UTF-8 preview (up to 256 KiB), not an editor.
Object hashes are checked when starting an analysis process; changed files are
refused and must be registered explicitly as another object/version.

## Verification

Multi-object orchestration tests cover five object delegations with only two
available analyst slots, out-of-order completion, queued cancellation, continued
tool use beyond the former 32-call limit, compatibility with obsolete `maxSteps`
settings, and preservation of completed tool results after interruption. Only
explicitly concurrent tool registrations overlap;
ordinary tool calls keep their ordering barriers. Audit-write failure prevents
the next invocation even when a concurrent batch is being admitted.

On 2026-11-08, the configured Responses model answered a Windows-message research
planning question in 8.72 seconds (two model requests, no object delegation). A
subsequent read-only check of `user32!GetMessageW` to
`win32u!NtUserGetMessage` completed in 43.70 seconds, with both object analysts
starting together, 14 model requests and 19 tool calls without tool errors. This
qualification established only that bounded static chain. Evidence remains
in the local `build-driverentry-lab/desktop-20261108/validation/agent-flow/`.
These timings describe the earlier qualification build; current runs have no
model-call count limit.

`NeverDAgent.Workspace` covers Unicode/space-containing paths, project reopen
and relocation, independent per-project object IDs, export/import provenance,
deduplication, mismatched-file and conflicting-record refusal, and malformed
project admission. Native window captures verify QML startup and rendering.
`NeverDAgent.WindowFrame` checks the absence of a native caption while retaining
the OS resize style, edge/corner hit tests (including maximized edges), and actual
maximize/restore/minimize transitions on Windows.
`NeverDAgent.Models` checks fragmented SSE, complete tool arguments, provider
continuations, encrypted credential round trips, actual local HTTP through NPH,
context persistence, cancellation and cross-project asynchronous updates. It also
forces pre-turn and in-task compaction, bounded summary requests, failed/empty
and cancelled summaries, checkpoint resume, whole tool-batch boundaries, and
provider/cache token accounting. `NeverDAgent.Markdown` exercises real document
structure, tables, code copying, incomplete fences and literal HTML/image labels.
`NeverDAgentLiveRunner` is an explicit integration-test executable (not installed
with the product). It reads JSON from stdin so credentials need not be placed in
command-line arguments or fixture files. It drives the same controller and
runtime as the GUI. `tests/fixtures/message_codec.c` supplies a small PE library
with checkable parser/checksum semantics for real ND/model acceptance.
Workspace tests additionally cover bundle/helper discovery, executable admission,
foreign absolute-path preservation, and hash-checked object relocation while
switching projects. Credential tests cover deferred settings commit, reload and
failed-commit cleanup. On macOS/Linux, native keyring integration tests require
an unlocked test-user keyring and explicit `NEVERD_AGENT_KEYRING_TESTS=1`; otherwise
only those native integration cases are skipped. They use synthetic secrets and
remove their own UUID entries, including after assertion failures.

Windows is the initially exercised platform; macOS/Linux packaging and
compositor behavior need native verification before release claims.

### Live qualification, 2026-11-07

DeepSeek's authenticated model listing confirmed `deepseek-flash` as
DeepSeek-V4.1-Flash. Real runs used the compiled `message_codec.dll` fixture,
without giving the model its source or executing the DLL:

| Protocol | Task | Observed run time | Result |
|---|---|---:|---|
| Responses | Packet parser, independent checksum subtask, saved reports and project summary | 151.89 s | Completed; 3 agent contexts, no tool errors |
| Messages | Checksum analysis through the object analyst and ND | 64.34 s | Completed; no tool errors |
| Chat Completions | Checksum analysis through the object analyst and ND | 58.20 s | Completed; no tool errors |

These are different tasks, not a controlled performance comparison. All three
use DeepSeek's compatible endpoints; live OpenAI/Anthropic accounts have not
been qualified. Evidence is under the local `build-driverentry-lab/
neverd-agent-20261106/live/` directory, including `acceptance-summary.json`, raw
per-project run logs and the generated reports. Checksum arithmetic, zero-length
behavior, five error categories and successful returns 1/2 match the fixture.
The reports' inferred business names (for example calling byte 2 a version)
are not established by the machine code. Model/tool round trips remain more
numerous than necessary; this qualification establishes a working analysis
path, not optimal agent efficiency.

The runs exposed and corrected two adapter/host issues: ND arguments were
double-encoded as JSON strings, causing repeated type errors, and long report
text hit an undisclosed 4096-unit host limit. ND arguments now travel as native
objects and text bounds are explicit. Output truncation also preserves its
reason and usage, with no partial tool dispatch.

The local Chinese-MSVC/CMake/Ninja setup additionally exhibited zero recorded
header dependencies (`ninja -t deps`), allowing old object layouts to survive
header edits. A complete fresh build removed the observed access violation and
passed the 17 focused Agent/Tools checks. Header dependency tracking in that
local toolchain still needs repair; use a complete rebuild for header changes,
and do not treat a no-op incremental build there as verification. The delivered
executable was checked byte-for-byte against the freshly built executable.

The 2026-11-07 Markdown/context build uses the Visual Studio 2022 generator
instead of that Ninja configuration. With a multi-config build, pass
`--config Release` to `scripts/build.py` and `cmake --install`.

### Context compaction acceptance (2026-11-07)

An isolated copy of the earlier `message_codec.dll` analysis was replayed through
DeepSeek Flash with a deliberately smaller 20,000-token context and 2,048-token
output reserve. Automatic compaction reduced the local input estimate from
16,390 to 2,465 tokens. Two bounded summary requests and one ordinary continuation
completed in 22.7 seconds, with no tool calls or repeated binary analysis.
The result retained the object hash, `ValidatePacket @0x180001030`, the conditions
for -4/-5, unconfirmed business fields and the existing report path. The final
provider reported 2,041 input + 851 output tokens; these are distinct from the
before/after local estimates. This checks useful continuation, not mathematically
lossless summarization. Original transcripts remain available for verification.

The regular user's DeepSeek context setting remains unchanged; only this
acceptance project used the small test window. Evidence is under the local
`build-driverentry-lab/neverd-agent-20261107/live-compaction/` directory.
