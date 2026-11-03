# Function report → reviewed name → project receipt

This external Python plugin demonstrates the model-independent Tools contract.
It is unrelated to TVM. It decodes a bounded entry instruction window through ND,
writes a hashed JSON artifact, then accepts an explicitly selected name through
a separate continuation action. The existing worker commits the rename to its
project journal. The deterministic caller reads the new name back through ND.

Requires Python 3.10+, an NPH host with Tools, and a matching `neverd-worker` /
engine package. Prepare a complete copy outside the NeverD repository:

```powershell
python pluginsdk/common/examples/tool-roundtrip/prepare.py E:/plugins/tool-roundtrip
python E:/plugins/tool-roundtrip/caller.py --host <bin>/neverd-plugin-host.exe `
  --worker <bin>/neverd-worker.exe --input E:/samples/test.exe `
  --function 0x140001000 --output E:/reports/tool-roundtrip-1
```

Use a disposable input copy for this demonstration: the caller explicitly
enables project writes and commits a name. Original machine bytes are unchanged.
The output directory must be new. `--name` selects a name; otherwise the caller
uses `reviewed_<address>`. Output includes instruction counts, the accepted
rename receipt, and the name read back in a fresh revision. Each operation also
has an atomic `receipt.json` and report artifacts under the output directory.

`plugin.json` declares ordinary process actions and parameter fields.
`tools.json` exposes those exact actions with output schemas, effects and the
explicit continuation relationship. Inputs reuse the action defaults and
validation. The two invocations run in separate Python processes; the follow-up
receives the host's retained report, not an old process closure.

The CLI consumer protocol supports `resolve`, `call`, `inspect`, `cancel` and
`release` JSON lines. A call must use a registration from its resolved set.
`cancel` requests process termination; accepted command receipts still drain.
Closing stdin disconnects the consumer and cancels work, then drains commands.
Releasing a set only prevents new calls. Repeating an identical call ID within
the same session/step returns the original operation; changed inputs conflict.

Without `--allow-project-writes`, discovery explains why `apply-name` is
unavailable. Editing the project after producing a report makes that report's
continuation stale, even if a new set is resolved. A new report is required.

This Tools catalog is loaded by `neverd-plugin-host tools`; the current GUI
process-plugin pane does not consume catalogs yet. A future GUI manager or
Agent adapter can consume the same `neverd_tools` library without moving this
plugin into the ND build. See `docs/tools-layer.md` in the NeverD source tree.
