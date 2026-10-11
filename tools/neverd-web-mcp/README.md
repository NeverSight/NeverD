# Native offline web MCP

`neverd-web-mcp` is a compiled C++ stdio server for NeverD's offline web analysis.
It calls the same Web C API adapter as `neverd-worker` directly. No worker
subprocess, Python/JavaScript runtime, sample execution or network listener is
part of this transport. See the [profile](../../docs/web-mcp-profile.md) for
the protocol, tool restrictions, budgets and current qualification limits.

Enable it in the normal repository build:

```sh
cmake -S . -B build -DNEVERD_BUILD_WEB_MCP=ON -DNEVERD_BUILD_SHARED=ON \
  -DNEVERD_ENABLE_WEB_ANALYSIS=ON -DNEVERD_ENABLE_PYTHON_PLUGINS=OFF
cmake --build build --target neverd-web-mcp
```

The embedded C++ JavaScript parser uses `NEVERD_ENABLE_WEB_JAVASCRIPT=ON`.
Optional archive dependencies follow the normal engine build profile.
`NEVERD_BUILD_WORKER` and the GUI are not required for this executable.

It can also build against a matching shared C ABI library without building
the engine or including LLVM headers in the transport:

```sh
cmake -S tools/neverd-web-mcp -B build-web-mcp \
  -DNEVERD_ENGINE_LIBRARY=/absolute/path/to/libneverd.dylib
cmake --build build-web-mcp
ctest --test-dir build-web-mcp --output-on-failure
```

Use the matching `.so` or `.dll` on other systems; Windows additionally needs
`NEVERD_ENGINE_IMPLIB`. The standalone tests require that library to match the
repository C ABI. A matching backend-disabled build is supported and advertises
only the capabilities tool. This is separate from the existing
[native binary Python MCP adapter](../neverd-mcp/README.md).

Configure an MCP client with explicit local inputs:

```json
{
  "mcpServers": {
    "neverd-web": {
      "command": "/absolute/path/to/neverd-web-mcp",
      "args": ["--stdio", "--input", "/absolute/path/to/sample"]
    }
  }
}
```

Repeat `--input PATH` up to eight times. Inputs may be files or directories;
startup does not read them. Call `neverd_web_import_preview` with
`{"input_index":0}`, inspect the metadata, and pass its `preview_token` to
`neverd_web_import_commit`. Subsequent tools take the returned revision and
artifact/source IDs. `neverd_web_capabilities` reports the available tools and
input count without disclosing configured paths. EOF ends the server.

Package the executable with the matching engine, its native dependencies and
all applicable engine licenses. JSON's MIT notice is staged at
`licenses/nlohmann-json/LICENSE.MIT` beside the executable and installed under
`share/neverd/licenses/nlohmann-json`. This target does not build a complete
cross-platform distribution by itself.
