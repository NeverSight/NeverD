# Offline model security scan

`neverd scan --gguf` inspects a local GGUF file or a local model package. It opens regular files read-only and reports evidence as `neverd.model-scan.v1` JSON. Ordinary `neverd scan` selects the native memory-safety analysis. Model inspection requires the explicit switch.

```sh
neverd scan --gguf ./weights.gguf
neverd scan --gguf ./model-directory -o new-report.json
neverd scan --gguf ./model.safetensors.index.json --model-offset=32 --model-limit=32
neverd scan --gguf ./weights.gguf --scale-bytes=1048576
neverd scan --gguf ./graph.onnx --model-text
neverd scan --gguf ./weights.gguf --rebuild-gguf=./verified.gguf --model-options='{"template_policy":"keep"}'
neverd scan --gguf ./candidate --baseline ./trusted-local-copy --compare-bytes=67108864
```

A file may have an arbitrary name when its bytes contain a GGUF header. A package directory selects its model members and recognized companions. A directory containing only model subdirectories selects each immediate package in sorted order. Standard numbered GGUF parts are grouped and checked against their split declarations. Different GGUF variants in one directory have separate inventories. Selecting a Safetensors shard reads that file and its companions. Select the index or directory to validate the complete shard mapping. The destination of `-o` must be a new file. Standard output is the default.

## Reading the report

| Field or view | Evidence |
| --- | --- |
| `artifacts` | Source identity, format, file size, and format-specific directory details. GGUF includes byte order, version, alignment and a SHA-256 of the serialized directory through `directory_end`. Safetensors includes a hash of its JSON header. |
| `metadata`, `templates` | Typed GGUF metadata with file coordinates and encoded value digests, plus template source, variant, length and decoded-text hash. Standalone and embedded text has file offsets. JSON strings use a field path and decoded UTF-8 offsets. Template bodies and arbitrary string values are redacted. |
| `tensors` | Package-relative artifact, exact tensor name, declared dimensions, logical element count, storage type, absolute byte offset and observed or calculated storage length. Quantized GGUF rows include elements and bytes per storage block. GGUF rows retain the serialized dimension order. A null length denotes an unverified range. |
| `declared_architectures`, `named_layer_tensor_counts` | A producer declaration and a grouping inferred from numbered tensor-name components. Each count measures serialized tensor entries in the group. |
| `inferred_projection_layouts` | Versioned profiles require four named rank-two matrices with known storage in the same numbered layer. Results count matching layers and link the example tensors to byte offsets. |
| `graphs` | Explicit ONNX graph and function scopes, operators, input/output value names, ports, nested graph references and initializer coordinates. Weight-only packages expose the inventory and named-layer view. |
| `findings`, `coverage_gaps` | Rule, category, source, evidence, confidence and review context. Gap reasons include malformed members, unsupported storage, an escaping path and budget exhaustion. Counts include rows beyond the retained evidence limit. |
| `file_bytes`, `bytes_read`, `tensor_payload_bytes_read` | Sum of sizes of the opened package members, bytes returned to the reader, and bytes read from weights. The comparison and optional block checks account for their payload reads. |
| `comparison` | Counts and per-tensor results for an explicitly selected local baseline, component changes, bytes compared and skipped, and compatibility or budget limitations. |

Exit **0** means the requested inspection completed without a review-level finding. Exit **2** means it completed with a review-level finding. Exit **1** covers partial coverage, cancellation, incompatible or incomplete baseline comparisons, and input or command errors. Inspect the status, findings and coverage together. A static report makes no prediction about model behavior or learned backdoors.

## Checks and limits

- **GGUF** covers little-endian v2 and little- or big-endian v3. The reader validates typed metadata, nested arrays, unique names, declared alignment, tensor dimensions, block widths, storage lengths and overlapping or truncated ranges. Unknown layouts produce a coverage gap. Structural checks require only the directory and bounded unreferenced ranges.
- **Padding and trailing data** have separate coordinates. The scanner checks required alignment bytes and reports unreferenced gaps and trailers. After the final tensor the report distinguishes possible alignment bytes from a trailer beyond the next boundary. A recognizable signature identifies bytes for review. The default unreferenced-byte budget is 4 MiB across the request. `--gap-bytes` adjusts it.
- **Optional quantization inspection** reads complete Q4_0 and Q8_0 blocks from verified ranges up to `--scale-bytes`. It records varying quantized codes under a zero scale (including the two Q4 nibbles), the sign-bit count for negative zero, the checked byte ranges, eligible and inspected block counts, and unsupported tensor counts. The default budget is zero. Conversion history matters when interpreting these findings.
- **Companion templates** include `chat_template.jinja`, `chat_template.json` and `tokenizer_config.json`. Embedded GGUF template variants use the same reader. Checks distinguish code tags, quoting, comments and raw blocks. External-template and private-attribute indicators are reported separately from prompt instructions and bidirectional override characters. Source conflicts compare decoded text for the same named variant. This is a bounded syntax review. The scanner never invokes a renderer or model runtime.
- **Safetensors** validates header bounds, duplicate keys, shapes, known dtype sizes, contiguous coverage and shard mappings. Model and processor configurations can declare custom loaders or request remote code. Package directories also inventory loader source, dependency declarations, native components and compiled-engine hints. Adapter configurations contribute LoRA declarations. References and filenames are data. Missing members and references outside the selected root produce diagnostics. ZIP and unframed checkpoints use the static instruction inspection described below.
- **ONNX** reads protobuf graph, node, opset, port, initializer and function fields. Length-delimited weights are skipped. Custom domains retain their declared identities. Local external-data references are checked against the selected root. With `--onnx-external-data`, the reader validates each local file, decimal offset, length and packed shape before making its range available. Inventory alone reads zero external weight bytes. Unadmitted external data, sparse and segmented storage, training graphs and unsupported typed initializer encodings carry coverage gaps. Attribute inventory includes field locations and graph-valued attributes.
- **Baseline comparison** defaults to exact tensor names and accepts a one-to-one analyst mapping bound to both snapshots. It checks declared architecture, representation, quantization and adapter declarations before streaming corresponding ranges through SHA-256. Float32 and float64 report finite pair counts and maximum finite absolute difference, with separate non-finite and overflow counts. Quantized tensors use serialized-byte comparisons. Whole tensors that do not fit the remaining read budget are skipped. Each byte on each side is charged once. The default weight budget is 64 MiB in total. Shard filenames do not determine correspondence.
- **Tokenizer and adapter comparison** includes configuration and template digests, tokenizer/vocabulary files, and adapter base, rank and target declarations. Changed, admitted JSON vocabularies report added tokens, removed tokens and reassigned IDs. File hash and decoded-ID evidence have separate fields. The caller supplies baseline provenance.
- **Resource accounting** includes a 64 MiB metadata budget per request, a 16 MiB individual JSON-header limit, a 1 MiB template limit, a depth limit of 64, and a 4 MiB serialized report limit. The default page size is 32 and the maximum is 512. `--model-offset` and `--model-limit` page tensor, GGUF metadata and ONNX node views. File identity, size and modification time are checked before publication and on later tensor reads.

In the workbench, open **Options → Analysis → Model Security Scan**, select a file or folder and choose **Scan**. Selecting an evidence row shows its JSON. The optional quantization sample size is in KiB and defaults to **Off**. The optional baseline field, paging, cancellation and report export use the same worker operation and C API as the CLI. The advanced JSON field accepts the same options as `--model-options='{"external_data":true,"tensor_range":{"name":"graph/weights","offset":0,"length":16}}'`. A tensor window returns its file offset, length and SHA-256. Windows must fit the validated storage and the 64 MiB range budget. Explicit CLI switches take precedence over embedded options. Member and package `snapshot_id` values bind local filesystem identities, sizes and modification times for invalidation. Content digests state their coverage separately. Tokenizer files are inventoried by default and hashed in full for baseline comparison when admitted.

## Structure and comparison

Use `--model-text` for a terminal listing. The JSON schema retains source offsets, nested graph and function IDs, symbolic dimensions, port slots and edges to visible enclosing values. A nonstandard domain retains the operator’s declared identity. Search tensors and operators with `{"search":"attention"}`. Select a graph via `graph_scope`. Use `graph_offset` to page the graph list and `offset` to page matching nodes and tensors. In the workbench, selecting a graph draws the current page. Expanding the row reveals operators and input edges. Selecting a node or edge opens its evidence. Export writes the backend’s serialized JSON and preserves 64-bit offsets.

`logical_elements` counts each serialized tensor entry. `identified_storage_bytes` sums validated ranges by entry. `unique_known_storage_bytes` counts the union of ranges per member and `shared_known_storage_bytes` counts repeated bytes. Declared weight tying remains a separate configuration fact. Projection hypotheses require a versioned rule and show supporting matrices. A named layer outside a declared layer count produces a diagnostic.

A comparison reports the candidate and baseline snapshots, a comparison ID, compatibility evidence, per-side byte and element counts, and the reason for each skipped range. To review a renamed tensor, substitute the two current `snapshot_id` values in the following options.

```json
{
  "tensor_mapping": {
    "candidate_snapshot": "<candidate snapshot>",
    "baseline_snapshot": "<baseline snapshot>",
    "names": {"layer.new_name": "layer.old_name"}
  },
  "sample_bytes_per_tensor": 4096,
  "absolute_tolerance": 0.00001,
  "relative_tolerance": 0.0001
}
```

Sampling uses deterministic prefixes aligned to complete storage units. Sample rows identify the digest as a prefix and retain the skipped count. For finite float32 and float64 pairs, the tolerance is the absolute tolerance plus the relative tolerance times the larger magnitude. Reports separate nonfinite pairs and unrepresentable absolute differences. LoRA matrix pairs expose their target, shapes and observed inner rank, with explicit missing-partner and declared-rank conflict states. A merged model can be compared as stored weights. Separate adapter contributions require evidence from an unmerged artifact.

## Checkpoints, loaders and provenance

- ZIP checkpoint scanning validates local records and the central directory, then decodes only admitted pickle members with size, work, expansion-ratio and CRC checks. Stored and raw-deflate members are supported. Unselected weight members retain coordinates and skipped-payload coverage. The seekable profile accepts an uncommented ZIP32 directory and a consistent classic-size ZIP64 trailer. Wide entries, alternate names and unsupported encodings produce diagnostics.
- The pickle reader inventories protocol 0–5 instruction framing. Object-construction instructions, global references, persistent IDs and external-buffer references carry opcode and byte coordinates. Archive coordinates distinguish stored bytes from decoded member offsets. Truncated streams and unknown instructions stop the affected inspection. The scanner never interprets the pickle stack or invokes a loader.
- The dependency view records configuration fields, local and missing loader references, remote references, and declared requirements. Exact pins and filename hints retain different evidence. Python scanning reports import tokens and specific loader call spellings, with comments and literal strings skipped. Source aliases, dynamic values and custom operator implementations require further review.
- Native PE, ELF and Mach-O components expose header evidence. Double-click a recognized native artifact to open the workbench’s static analysis. Engine filenames identify research inputs. Model-card and configuration claims, license content and available signature files appear under provenance. A signature with no verification result is labeled `present-unverified`. Remote references and unparsed dependency expressions are represented by digests.

## Verified GGUF reconstruction

Select a standalone GGUF file and choose **Rebuild GGUF…**, or pass `--rebuild-gguf=NEWFILE` with `--gguf`. The required JSON `template_policy` is `keep`, `remove` or `replace`. Removal and replacement select an existing embedded `tokenizer.chat_template` field by default. A named variant can be selected with `template_key`. Replacement takes the exact UTF-8 `template_text` supplied by the caller.

Reconstruction streams each understood tensor into checked, aligned ranges, discards verified unreferenced storage and writes zero padding. A second parse checks metadata, tensor names, dimensions, types, offsets and every payload digest. The audit includes complete input and output SHA-256 values, tensor coordinates, changed fields, input unreferenced ranges and output padding. Publication requires a new path on a filesystem supporting hard links. The input and output each have a 64 GiB limit. Split inputs, unsupported storage, mutation, cancellation, failed verification or an oversized audit prevent publication. Companion templates have independent artifact identities and the operation applies to the selected GGUF.

The C entry points are `neverd_model_scan_json_v1` and `neverd_model_rebuild_gguf_json_v1`. Both return an owned UTF-8 report released with `neverd_free_string`. The worker operations are `model_scan` and `model_rebuild_gguf`. Inspection uses a local path and the same JSON options as the CLI. Reconstruction also requires a new output path and an explicit template policy.

## Measured local inputs

The Release CLI on an Apple M4 Max produced these results with the default budgets. File bytes sum the members opened during each command. Reader bytes include the inspected headers, companions and padding. Peak RSS is the process maximum reported by macOS `/usr/bin/time -l`.

| Input | Tensors | File bytes | Reader bytes | Weight bytes read | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| Two MLX packages, 3 and 6 Safetensors shards | 2,180 + 2,180 | 45,556,230,966 | 1,030,006 | 0 | 21.0 MiB |
| StarCoder2 3B, GGUF v3 | 483 | 1,709,888,576 | 1,757,993 | 0 | 18.5 MiB |

Separate readers checked every tensor name, shape, storage type, absolute offset and length, together with the Safetensors shard mapping and both 8,952-byte template hashes. On the GGUF, a separate `--scale-bytes=65536` run checked 3,640 complete Q4_0 blocks (65,520 bytes) and reported prefix sampling. Comparing the two MLX variants found incompatible quantization declarations and recorded zero bytes compared. [GGUF cross-check implementation, v0.28.4](https://github.com/gpustack/gguf-parser-go/tree/v0.28.4).

The format contracts are maintained in the [GGUF specification](https://github.com/ggml-org/ggml/blob/master/docs/gguf.md), [GGML storage definitions](https://github.com/ggml-org/llama.cpp/blob/master/gguf-py/gguf/constants.py), [Safetensors specification](https://github.com/safetensors/safetensors) and [ONNX wire schema](https://github.com/onnx/onnx/blob/main/onnx/onnx.proto3).
