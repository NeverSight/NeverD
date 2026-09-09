# NeverD MCP

`neverd-mcp` is a Python 3.10+ standard-library adapter. It has no Qt, LLVM,
third-party Python dependency, network listener, model provider, or agent loop.
It implements MCP **2025-11-25** JSON-RPC over newline-delimited stdio; its
internal worker connection uses the separate length-prefixed worker protocol.
The executable, `server.py` and `transport.py` must stay in the same directory.
The macOS application also supplies a launcher at
`NeverD.app/Contents/MacOS/neverd-mcp`. It uses the bundled interpreter when the
engine includes Python.framework; otherwise it requires Python 3.10+ on `PATH`.
The application GUI itself does not need Python when Python engine plugins are
disabled. The launcher still requires an explicit worker/file or attach path.

## Headless use

Configure your MCP host with an explicit executable and arguments, for example:

```json
{
  "mcpServers": {
    "neverd": {
      "command": "/absolute/path/to/python3",
      "args": [
        "/absolute/path/to/NeverD/tools/neverd-mcp/server.py",
        "--worker", "/absolute/path/to/neverd-worker",
        "--file", "/absolute/path/to/your-binary"
      ]
    }
  }
}
```

The adapter starts exactly one explicitly selected worker and opens only that
input with `read_only:true`. MCP callers cannot select other files, load plugins,
edit annotations, run targets or launch commands. Decompilation and some queries
may trigger the existing full-image analysis; read-only means no persistent
project edits, rather than no internal analysis work. Closing MCP terminates its
owned worker. Use the worker that ships with the matching engine build.

Tools: `project_metadata`, `list_functions`, `read_disassembly`,
`read_decompilation`, `read_bytes`, `list_xrefs`, `list_strings`, `read_cfg`.
All addresses are exact hexadecimal strings. Results preserve the worker's
`project_id`, `revision`, status, page continuation and original addresses.
Use `expected_revision` when continuing a page; an analysis-triggering query can
publish a newer revision. `resources/list` provides metadata and the first
function page. `resources/templates/list` describes parametrized function,
disassembly, C/IR and xref resources.

Results larger than 512 KiB return a `resource_link` and `resource_uri` pointing
to a session-local immutable snapshot, such as `neverd://result/1`. Read it with
`resources/read` and `?offset=0&limit=32768`; the response contains JSON text
chunks, character offsets, `next_offset`, `complete`, project ID and revision.
Concatenate those chunks to recover the original JSON. Continuations read the
cached revision without querying the worker again. At most four snapshots and
8 MiB of encoded snapshot data are retained; eviction is explicit when read,
and callers can repeat the original query to create a new snapshot.

## Attach to the GUI

Explicitly enable **Share current session** in the GUI, then use the displayed
private credential file:

```sh
python3 /absolute/path/to/server.py --attach /private/session/credentials.json
```

The broker is enabled only by the user. It uses an OS-user-scoped local socket
or Windows named pipe plus a random session token, routes queries to the GUI's
existing worker, and closes connections when sharing is disabled or the GUI
exits. It does not reopen the project or fall back to a new worker. Credentials
are temporary, owner-only files and are removed when sharing ends. Keep that
file private. Opening another project or replacing the worker session revokes
sharing; enable it again explicitly for the new session. GUI attachment additionally exposes `current_selection`,
`navigate_gui`, `highlight_gui` and `neverd://gui/selection`. Navigation is an
explicit tool call; resources never change selection. Highlighting selects the
address in the current workbench, while navigation also updates function views.

## GUI MCP client

The GUI's MCP panel accepts a manually chosen local stdio executable and argument
array, or a Streamable HTTP URL. Connections initialize only when requested;
tool calls require explicit user action and their results are displayed as data.
The history keeps at most 50 calls, preserving active calls for cancellation.
Arguments and result previews are limited to 4096 characters each and indicate
truncation. Inspecting history displays the saved record without repeating the
call. Cancelling sends a protocol notification, closes an HTTP response stream
if present, and ignores late responses; remote synchronous work may continue.
There are no sampling, model, shell, browser, or autonomous workflow capabilities.

HTTP accepts HTTPS with normal certificate verification; HTTP is allowed only
on loopback. A manually provisioned bearer token and an optional additional CA
certificate file are supported. The token is held in memory and is not saved.
Automatic OAuth discovery/registration/login is not implemented; configure a
server accepting your pre-provisioned token. Credentials are never forwarded
through redirects. Responses may be JSON or SSE. Session IDs and negotiated
protocol headers are sent on subsequent requests, expired sessions initialize
again, and resumable SSE responses retry at most three times. The client never
automatically repeats a tool call after session expiry. Unsolicited resource
subscriptions and legacy HTTP+SSE transports are not implemented.

## Budgets and cancellation

- MCP input: 1 MiB per newline-delimited message; worker frames: 8 MiB.
- Adapter queue: 16 messages, one serialized worker query; GUI client: 32 requests.
- Inline tool/resource output: 512 KiB. Larger results become bounded cached
  resources; over 8 MiB returns an explicit budget error. Pages are at most 512 entries,
  bytes at most 4096, and CFG size remains bounded by the worker.
- Request timeout: 120 seconds. GUI broker: at most four authenticated clients,
  16 pending requests per client and 32 total.
- Cancellation suppresses queued/cancelled MCP results. An already running
  synchronous analysis can continue until completion; the adapter does not claim
  to stop engine computation. A worker timeout ends the connection.

These are transport/result budgets, not guarantees about LLVM analysis memory.
The GUI client's server capabilities list currently shows its first page and
marks a further server cursor as partial. Tool results are never interpreted as
instructions or automatically fed to a model.

## Verification

```sh
python3 -m unittest discover -s tools/neverd-mcp/tests -v
cmake -S tools/neverd-gui/mcp/tests -B build-mcp-tests -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build-mcp-tests
ctest --test-dir build-mcp-tests --output-on-failure
```

Tests exercise version negotiation, initialization ordering, exact addresses,
scope and argument validation, resource paging, result errors/budgets,
fragmented worker frames, GUI credentials and same-session routing, subprocess
stdio, request cancellation/history limits, malformed or oversized servers,
cached snapshot paging/eviction, and HTTP session/auth headers with fragmented
and resumed SSE. Build the Qt tests
inside the checkout on macOS; Qt's generated relative MOC includes can be wrong
when the build directory goes through the `/tmp` symlink.

Protocol references: [MCP transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports),
[MCP lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle).
