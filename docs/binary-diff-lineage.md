# Offline version comparison graphs

Combine saved `.nddiff` reports to trace reported function associations across
analyzed snapshots. The `lineage` command, Python `Lineage`, C ABI and desktop
**File → Version comparison graph** use the same engine. Native input files are
not required after reports have been saved.

```sh
neverd lineage --report release-a-b.nddiff --report release-b-c.nddiff \
  --report release-a-c.nddiff --save releases.ndlineage --json
neverd lineage --reopen releases.ndlineage --list versions --json
neverd lineage --reopen releases.ndlineage --snapshot SNAPSHOT_ID \
  --address 0x1000 --list trace_nodes --limit 100 --json
neverd lineage --reopen releases.ndlineage --list trace_edges \
  --snapshot SNAPSHOT_ID --address 0x1000 --offset 0 --json
neverd lineage --reopen releases.ndlineage --export-report REPORT_ID \
  --save original.nddiff
```

Use IDs returned by `versions` and `reports`. Function addresses are exact entry
addresses. `--reopen` can be combined with more `--report` arguments. `--save`
writes the whole collection, except with `--export-report`, when it writes the
exact original source bytes. Output files are replaced only after successful
preparation and the final cancellation check. A stdout failure after file
publication cannot roll back the file.

### Open a report from an older analysis profile

If `diff --reopen` reports that the inputs were analyzed with other settings,
open the saved evidence offline:

```sh
neverd lineage --report review.nddiff --save history.ndlineage --json
neverd lineage --reopen history.ndlineage --list reports --json
```

In the desktop, choose **File → Version comparison graph**, then add the saved
comparison. Both routes retain its original evidence and report bytes. They do
not require the original binaries. To compare the binaries using the current
analysis profile, create a new comparison and save it to a separate file. Fresh
binding validates the analysis identity and function evidence; changing a
profile or revision label alone does not migrate a report.

The desktop opens an independent worker. Add comparisons, label snapshots and
branches, filter functions, activate a function to trace it, inspect direct
source evidence and conflicts, then save or reopen the collection. Tables retain
one bounded page. The window uses the selected interface language, including
right-to-left layout and navigation for Arabic. Original evidence, function
addresses and saved identifiers retain their original values.
**Rows** chooses each table's page size; reduce it when large
labels or evidence exceed the page's byte allowance, then continue with
Previous/Next through the complete admitted inventory. Changing the page size
retains its starting offset and retries the query without changing the collection.
The diagram shows the current source-report page, not an entire unbounded graph.
Fit shows the complete page; Zoom in remains available after fitting a dense
diagram into a small window.
Keyboard activation and Copy copy original row evidence; context menus also copy
full values. Long labels shorten only for display; table cells, headings and graph nodes
use a single line for the label. Headings retain their full
accessible text and tooltips, and copied row evidence retains the original label.
Adding comparisons and editing metadata retain the selected snapshot, page,
filter and trace. Opening a replacement collection starts on its first snapshot
with an empty filter and trace; failed replacement retains the current navigation.
Resource settings apply to subsequent loads and queries. Invalid,
stopped or over-budget replacement leaves the old collection available.

## Evidence and identity

A snapshot is one binary content hash **and exact analysis identity**, including
project identity, format, architecture, ABI, normalization, revision, analysis
settings, endianness, bitness, image base, entry and discovery completeness. Only
the caller's filesystem path is excluded. Different inventories under the same
identity are refused. Identical content analyzed identically is one snapshot;
separate release records may refer to that same snapshot. Snapshot labels and
branches are analyst-authored metadata, not evidence of ancestry.

Active byte patches contribute their effective file offsets and bytes to the
analysis identity, including data outside decoded functions. The underlying
file's SHA-256 remains unchanged. The overlay component ignores edit kinds and
row partitioning; reverting every changed byte removes that component. Saved
comparison binding therefore refuses an earlier unpatched analysis even when
the decoded function inventory is unchanged. Packed project state remains an
additional, independent part of snapshot identity.

Materialized analysis sections also contribute their complete bytes, mapping,
permissions and declared functions to analysis identity, including data outside
the selected functions. Section and function declaration insertion order does
not matter. Removing every added section removes this identity component.
Reopening a saved comparison requires the same additions to be present in the
current input; the original file hash keeps its original meaning.

An edge retains one accepted pair from one source report. Traversal is undirected;
the original comparison direction remains in its evidence. Direct, indirect,
reverse, cyclic and parallel report paths remain visible. A shortest predecessor
witness is supplied without discarding the other edges. Conflicting counterparts
in the same snapshot, and an effective report-local Unmatch reached through
other reports, produce explicit conflict groups. No winner is selected. Historical decisions referencing an absent function stay
visible with inventory flags, a warning page entry and a summary warning count;
they never create function nodes.

Ambiguities, decision histories and native/SSA region hypotheses remain separate
from accepted edges. Native region anchors contain original snapshot **indices**,
not freshly resolved addresses. SSA previews expose counts and frontiers; the
exact source contains complete node mappings. Invalid anchor references fail
explicitly. Manual assessments retain their original report, case, pair, author,
note and supersession. They never propagate a patch verdict along graph paths.

A source digest binds exact report bytes, including whitespace. It authenticates
neither an author nor native inputs. Historical `analyst-review-v1` digests retain
their original field coverage; newer project metadata is not retroactively added
to that journal format. `native_authenticated` remains false. Complete traversal
means all admitted associations were traversed, not semantic equivalence, patch
presence or vulnerability elimination. Incomplete source reports stay explicit.
The **Direct evidence** table also shows each pair's change state. An empty
observed-change list with unresolved reference identities is **Uncertain**;
it is not presented as unchanged. Copied edge evidence retains the separate
decoded-body and reference-comparison coverage fields through save and reopen.

## Queries and limits

Every C ABI query requires `kind` and the exact collection `revision`. CLI, worker
and Python fill the current revision only when omitted; an explicit stale value
is refused. All pages return `items`, `total`, `offset`, nullable `next_offset`,
`revision`, `kind` and charged `work`. Unused or unknown query fields are refused.

| Kind | Additional selectors |
| --- | --- |
| `versions`, `reports`, `edges`, `conflicts` | None |
| `functions` | `version`, optional name/address `search` |
| `trace_nodes`, `trace_edges`, `trace_conflicts` | `version`, string `address` |
| `conflict_members` | Conflict `index` |
| `reviews` | Optional `report`; `latest: false` includes superseded history |
| `decisions`, `ambiguities`, `warnings`, `native_regions`, `flow_regions` | `report` |
| `ambiguity_primary`, `ambiguity_secondary` | `report`, ambiguity `index` |
| `native_region_anchors`, `flow_region_anchors` | `report`, hypothesis `index` |
| `native_anchors`, `flow_anchors` | `report`; all saved spans, including unclassified evidence |
| `releases` | Optional `version`, `search`, or exact release ID `focus` |
| `release_parents`, `release_children` | `release` ID |

Release pages contain parent and child counts. Their separate relation pages
avoid putting an entire parent inventory in one row. `focus` finds the page
containing an exact release ID under the current filter and row allowance; it
cannot be combined with `offset`. The returned `offset` is authoritative. This
keeps the selected release reachable after its dates or parents change its place
in the timeline.

Use `offset`, `limit`, `max_work` and `timeout_ms` on any query. Default pages
contain 200 rows; the shared maximum is 4,096 rows and 8 MiB. An oversized page
fails instead of silently truncating. Reduce its row allowance or export the
original report when individual evidence exceeds a page. CLI `--query` accepts
this object directly; ordinary page options cannot be mixed with it or with
source export. `--history` selects superseded review history.

Admission options are `max_source_bytes`, `max_nodes`, `max_edges`, `max_work`
and `timeout_ms`. Defaults are 256 MiB retained report/label text, 1,048,576
function/release records, 4,194,304 association/parent-link records and
2,147,483,648 work units.
A zero deadline means no deadline. The deadline begins inside the native engine
operation; preceding CLI/worker file reading and bundle preparation are
cancellable but are outside that deadline. It is not a whole-command wall-clock
timeout. Admission accounts for the old collection,
candidate and retained trace during replacement. Exact validated source records
may be reused, but cache hits are charged against current allowances. One complete
trace is cached by collection revision and source node, independently of page
number and trace page kind. Current work/deadline settings still apply.

Source-byte accounting is not process RSS. Work counts lineage byte/record/walk
operations, not ordered-index comparisons or all hidden saved-reader verifier
operations. The saved reader retains its own verification budgets. JSON parsing,
allocations and whole-value copies/comparisons are indivisible cooperative
checkpoints. Local file input has an explicit aggregate 2 GiB bound. Whole
collections and reports remain in the worker; only bounded pages cross IPC.

CLI exits: `0` complete without conflicts, `2` conflicts, `3` incomplete source
reports (takes precedence over conflicts), `1` invalid input or I/O failure,
`130` cancelled, `124` timed out, `4` resource limited. Inspect the summary to see
both conflicts and incomplete sources. A stopped load/query produces no partial
stdout or replacement file. Late cancellation does not undo publication.

## Python and persistence

```python
from pathlib import Path
from neverd_plugin import Lineage

with Lineage() as graph:  # Inside a Python-enabled NeverD plugin.
    graph.load_reports([Path("a-b.nddiff").read_bytes().decode("utf-8"),
                        Path("b-c.nddiff").read_bytes().decode("utf-8")])
    for page in graph.pages("versions", limit=100):
        print(page["items"])
    Path("releases.ndlineage").write_text(graph.export())
```

Outside the plugin host, pass `host=HostAPI(ctypes.CDLL(library_path))` using
`HostAPI` from `neverd_plugin.ffi` and the matching native library.

Use binary-preserving UTF-8 reads (`read_bytes().decode("utf-8")`) when exact
source line endings matter; ordinary text-mode reads may normalize them before
admission. `load`, `load_reports`, `query`, `export` and `report` support progress
callbacks. Callback exceptions cancel native work and are reraised after it
returns. Only `status` and `cancel` may overlap an operation. Close the handle
after active work returns. Missing optional native exports fail before creating
a handle. `pages()` fixes one revision for the entire iterator.

A `.ndlineage` v1 document has `schema: "neverd.bindiff.lineage"`,
`schema_version: 1`, `reports` containing exact JSON strings, and `versions`
containing `{id, label, branch}` metadata. Reload revalidates source reports and
reconstructs the graph; derived edges or conclusions are not trusted from disk.

## Declared releases and timelines

A release is a named occurrence of an analyzed snapshot. Two releases can share
the exact same snapshot, while remaining separately visible in a branch or
release timeline. Release records contain:

```json
{
  "id": "stable-1.2",
  "label": "Stable 1.2",
  "branch": "stable",
  "version": "SNAPSHOT_ID_FROM_VERSIONS",
  "published_at": "2026-10-25T08:00:00Z",
  "parents": ["stable-1.1"]
}
```

`id` is a stable, nonempty release identifier; labels need not be unique.
`published_at` is either null for an unknown time or a valid UTC timestamp in
exact `YYYY-MM-DDTHH:MM:SSZ` form. Offsets, fractional seconds and leap-second
notation are not accepted by this profile. A release can have no declared
parents, one parent, or multiple parents for a merge. Parents must already exist
or be created in the same edit. Cycles, repeated parents, self-parenting, missing
snapshots and dangling parent references are refused. A known publication time
cannot precede any known ancestor time, including through undated records.

These are analyst declarations. The engine does not infer ancestry from report
direction, filenames, branch labels, matching functions or timestamps. Timeline
rows place declared parents before children. Among currently available rows,
known UTC times precede unknown times, then IDs break ties. Unrelated adjacent
rows do not thereby acquire a parent relationship. Function correspondence and
patch assessments retain their original report-local meanings.

Use a release edit file to create, relabel or remove records:

```sh
neverd lineage --reopen releases.ndlineage --release-edit edit.json \
  --save releases.ndlineage --list releases --json
neverd lineage --reopen releases.ndlineage --list release_parents \
  --release stable-1.2 --limit 100 --json
```

An edit object contains the exact `revision` plus any of `upsert`, `remove` and
`parent_updates`. CLI, worker and Python fill the current revision only when
omitted; an explicitly stale revision fails. Upsert records contain the fields
shown above. For an existing release, omitting `parents` preserves all parents,
including those outside the current page. New records require an explicit
parents array. A bounded parent edit needs only the changed links:

```json
{
  "parent_updates": [
    {"release": "stable-1.2", "add": ["hotfix-1.1.1"], "remove": ["stable-1.1"]}
  ]
}
```

The owner first applies record upserts/removals, then parent deltas, then validates
the complete candidate. Duplicate record edits, IDs both removed and upserted,
repeated parent edits, removing an undeclared parent and adding an already
declared parent are errors. Removing a referenced release requires removing its
child links in the same transaction. No intermediate graph becomes visible.
Edits share load options, cancellation, retained-trace accounting and atomic
publication. They reuse the validated source evidence and copy the unchanged
association graph; work covers the edit, graph copying and release validation
without serializing or reparsing the original reports. Node, edge and source-byte
allowances still account for the old collection and candidate held together.
Their request object has the shared 1 MiB options allowance; large
inventories remain editable through bounded deltas or whole-file loading.

Python exposes `graph.edit_releases(edit, options=..., progress=...)`; the C ABI
provides the additive optional `neverd_lineage_edit_releases_json` export. Missing
that export leaves the original load/query/save workflows available. The worker
advertises `lineage_edit_releases` only when the linked engine supports it.

In the desktop's **Releases** tab, add or edit a release, choose its snapshot,
and browse declared parents and children through bounded tables. Parent selection
uses labels and IDs, so duplicate labels remain distinguishable. Editing metadata
retains parent links unless explicitly changed. Successful edits focus the changed
release and reset the release filter; deleting the final item on a page returns
to the first page. Dates remain optional. Opening the associated snapshot returns
to function inspection. Save the graph to persist these edits.

Release editing upgrades the saved document to v2 with an additional `releases`
array. V1 files keep their original revision derivation and persistence version
until upgraded. Removing every release from v2 does not downgrade it. Older
readers refuse v2 explicitly. Summary, page and status projection versions remain
v1; summaries expose `document_schema_version` separately. Original `.nddiff`
bytes, snapshot identities, decisions and assessment histories are unchanged.

This profile does not complete the [advanced acceptance map](binary-diff-acceptance.md).
Package-wide graphs, automated patch/backport/revert evidence,
and independent large-corpus quality/latency qualification remain separate work.

The summary advertises `supports_all_region_anchors: true` when these all-span
queries are available. Desktop clients use this capability before requesting
them; older engines retain their existing source-evidence pages.
