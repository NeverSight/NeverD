# External SBF corpus manifests

`solana-dex-2026-10-24.json` pins 72 `.so` release assets from
`cpkt9762/solana-dex-program-dumps` at commit
`7d5d1f424ad326e332b69af614d9849bc397298b`, release ID `423249665`.
It stores metadata and checksums, without binary blobs.

Use `scripts/prepare_sbf_corpus.py` to verify a local cache; downloading requires
`--fetch`. The snapshot asset URLs are intentional: the metadata at that collector
revision still references older individual-program releases.

The recorded execution slot is unknown. Loader ownership and the selected
analysis profile remain separate from the deployment slot. See
`docs/sbf-corpus.md` for qualification commands and evidence limits.
