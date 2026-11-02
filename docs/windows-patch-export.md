# Windows patch export and temporary-file ownership

## Cause and reproduction

The patched-binary exporter writes an LLVM `TempFile`, copies the input's
permissions by path, then creates a hard link to a new destination. On Windows
LLVM creates that temporary file with delete disposition on its open handle.
Both the permission edit and `CreateHardLinkW` can reject this delete-pending
source with `ERROR_ACCESS_DENIED`; permissions are the first failing boundary
in the real exporter. Replacing only the hard-link operation leaves that earlier
failure intact. A writable output directory does not fix this lifecycle error.

With a built native worker, run the generated ELF and PE regression:

```powershell
python tools/neverd-worker/tests/byte_patch_test.py <build>/bin/neverd-worker.exe --real
```

The PE case uses `pe_browse_test.image()`, opens the input with `analysis:false`,
applies byte `2b` over expected `2a` at `0x140001002`, and exports to a previously
nonexistent path in the same directory. Before the repair `patch_export` returns
`export_failed: Access is denied.` after preview, application and history have
already succeeded. The test needs no external binary or plugin.

## Shared repair

`support::atomic_output::closeAndCommitTemporaryOutput` owns cleanup registration,
the writing descriptor and the retained Windows handle. Its explicit
`OutputMode::CreateNew` retires delete disposition before copying optional
permissions, closes the writing descriptor, then publishes through one
`FileRenameInfo` request with `ReplaceIfExists = FALSE`. An existing destination,
including the input path or an alias, must remain unchanged. Failure consumes
the temporary file. The default replacement mode retains its existing contract.

On POSIX exclusive publication uses a hard link followed by temporary-path
removal. Cleanup errors after successful publication are reported; a caller
must not infer from such an error that no destination was created.

`PatchExport.cpp` delegates permissions and publication to this owner. Original
byte replay, checksum calculation and explicit consent for invalidating a
signature remain in the patch exporter.

## Verification

Build the focused targets using the supported repository entry point:

```powershell
python scripts/build.py build --target NeverDBytePatchTests NeverDTranslationContractTests neverd-worker
build/bin/NeverDBytePatchTests.exe
build/bin/NeverDTranslationContractTests.exe --gtest_filter=TranslationCLIContract.*
ctest --test-dir build -R '^NeverDWorkerNativePatch$' --output-on-failure
```

The patch tests cover exclusive publication, unchanged inputs/destinations,
directory refusal, temporary-file cleanup and existing PE/Mach-O signing rules.
The translation contract suite covers default replacement, locked destinations,
close/registration failures and cleanup. The real worker additionally covers
UTF-8 PE paths, history/replicas, save/reopen and ELF/PE export.

Windows runtime results accompany the pull request. POSIX runtime qualification
requires a POSIX host; Windows coverage does not establish it.
