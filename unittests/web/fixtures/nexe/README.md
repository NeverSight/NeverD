# Controlled nexe producer fixtures

`manifest.json` records independent plaintext inputs and hashes of three full
producer outputs. Native binaries and upstream bootstrap payloads are not
redistributed. A corpus root contains `linux-x64-fixture`, `mac-x64-fixture`,
and `windows-x64-fixture.exe`. Set `NEVERD_NEXE_4RC7_CORPUS` to that root when
running the ordinary C++ tests. No fixture is ever executed.

Preparation is a **manual development activity**. It does not add Node/npm/nexe
as an analyzer or test dependency. Build `neverd-web-nexe-fixture-inputs`, then
run it with this manifest and a new directory to reproduce the four inputs.
The original producer run used Node 24.1.0, npm 11.3.0, macOS arm64 and an
isolated `npm install --ignore-scripts --no-audit --no-fund nexe@4.0.0-rc.7`.
Its dependency-lock hash is recorded; changed transitive dependencies are not
silently treated as the same qualification.

Download the three named Node 14.15.3 bases from
[`nexe/nexe` release v3.3.3](https://github.com/nexe/nexe/releases/tag/v3.3.3)
and verify the manifest's SHA-256 values. From the input directory, invoke the
pinned producer once for each target with explicit local paths:

```text
node /ABSOLUTE/PRODUCER/node_modules/nexe/index.js main.js --no-bundle \
  --resource asset.txt --resource minified.js --resource package.json \
  --target TARGET --asset /ABSOLUTE/BASE --output /ABSOLUTE/OUTPUT
```

Targets: `linux-x64-14.15.3`, `mac-x64-14.15.3`, `windows-x64-14.15.3`.
Outputs must match the recorded full-file hashes. Run NeverD's `web nexe`
command with `nexe-4.0.0-rc.7-eof-resources-v1`, or the optional C++ cases, to
compare all member bytes to the independent manifest inputs. The fixture-input
writer itself contains no packager invocation. See the [profile](../../../../docs/web-nexe-profile.md)
for support boundaries, native-prefix/signature limitations and budgets.
