"""External bytecode rules routed through the shared NeverD recovery API."""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
import json
from typing import Any, Callable, Literal, Mapping, Sequence, cast

from . import abi
from .api import NeverDError
from .ffi import HostAPI


@dataclass(frozen=True)
class BytecodeRecoveryResult:
    """Reachable coverage and optional state-ABI C, not original source types."""

    functions: int
    blocks: int
    decoded_instructions: int
    decoded_bytes: int
    input_bytes: int
    scope: Literal["cfg", "state-c"]
    source: str
    with_context: bool = False
    mapped_observations: int = 0
    unmapped_observations: int = 0
    reconstructed_calls: int = 0
    unresolved_contract_calls: int = 0
    guarded_contract_calls: int = 0


def recover_bytecode(
    code: bytes,
    profile: Mapping[str, Any],
    functions: Sequence[Mapping[str, Any]],
    *,
    bindings: Sequence[Mapping[str, Any]] = (),
    base: int = 0,
    output: Literal["check", "highc", "llvmc"] = "highc",
    optimize: bool = False,
    unaligned_pointers: bool = False,
    with_context: bool = False,
    observations: Mapping[str, Any] | None = None,
    source_contracts: Mapping[str, Any] | None = None,
    api: HostAPI | None = None,
) -> BytecodeRecoveryResult:
    """Check or emit caller-defined bytecode through the public C ABI.

    A trusted plugin owns container reading and rule construction. The engine
    validates encoding, CFG and state lowering; it does not execute the input.
    Dynamic decoder contexts and original source ABIs are not inferred by this
    profile API. ``api`` can name an explicitly loaded library outside the host.
    ``with_context=True`` adds a caller-owned opaque ``void *context`` to every
    emitted function and bound call; it does not execute callbacks or store a
    Python object in the output. Its lifetime belongs to the generated C caller.
    """
    return _recover(
        code, profile, functions, bindings=bindings, base=base, output=output,
        optimize=optimize, unaligned_pointers=unaligned_pointers, api=api,
        with_context=with_context, observations=observations,
        source_contracts=source_contracts,
    )


def recover_bytecode_with_decoder(
    code: bytes,
    layout: Mapping[str, Any],
    functions: Sequence[Mapping[str, Any]],
    decoder: Callable[[bytes, int], Mapping[str, Any]],
    *,
    bindings: Sequence[Mapping[str, Any]] = (),
    base: int = 0,
    output: Literal["check", "highc", "llvmc"] = "highc",
    optimize: bool = False,
    unaligned_pointers: bool = False,
    with_context: bool = False,
    source_contracts: Mapping[str, Any] | None = None,
    api: HostAPI | None = None,
) -> BytecodeRecoveryResult:
    """Recover with a synchronous, trusted Python instruction decoder.

    ``decoder(byte_window, pc)`` receives up to 4096 bytes starting at PC,
    truncated at the function end, and returns a mapping with
    ``size`` and ``operations`` using the profile operand grammar, without
    ``match``. It may instead return ``{"error": "diagnostic"}`` or raise.
    ``layout`` contains the profile's bank sizes and byte order, no encodings.
    Results must depend only on bytes, PC and stable closure context, never on
    CFG visit order. The decoder is not retained after this call. Exceptions
    are re-raised after the native response has been released; none crosses
    the C callback boundary. This does not execute input or infer source ABI.
    ``with_context`` selects the emitted source ABI as in ``recover_bytecode``;
    that runtime context is separate from this instruction decoder's closure.
    """
    if not callable(decoder):
        raise TypeError("decoder must be callable")
    return _recover(
        code, layout, functions, decoder=decoder, bindings=bindings, base=base,
        output=output, optimize=optimize,
        unaligned_pointers=unaligned_pointers, api=api,
        with_context=with_context, source_contracts=source_contracts,
    )


def _json_bytes(value: Any, label: str) -> bytes:
    text = json.dumps(value, allow_nan=False, separators=(",", ":")).encode("utf-8")
    if len(text) > 64 * 1024 * 1024:
        raise ValueError(f"{label} exceeds the 64 MiB limit")
    return text


def _recover(
    code: bytes,
    rules: Mapping[str, Any],
    functions: Sequence[Mapping[str, Any]],
    *,
    bindings: Sequence[Mapping[str, Any]],
    base: int,
    output: Literal["check", "highc", "llvmc"],
    optimize: bool,
    unaligned_pointers: bool,
    with_context: bool,
    api: HostAPI | None,
    observations: Mapping[str, Any] | None = None,
    source_contracts: Mapping[str, Any] | None = None,
    decoder: Callable[[bytes, int], Mapping[str, Any]] | None = None,
) -> BytecodeRecoveryResult:
    if not isinstance(code, bytes):
        raise TypeError("code must be bytes")
    if len(code) > 64 * 1024 * 1024:
        raise ValueError("code exceeds the 64 MiB limit")
    if type(with_context) is not bool:
        raise TypeError("with_context must be a boolean")
    request_fields: dict[str, Any] = {
        "schemaVersion": 1, "layout" if decoder is not None else "profile": dict(rules),
        "functions": [dict(item) for item in functions],
        "bindings": [dict(item) for item in bindings], "base": base, "output": output,
        "optimize": optimize, "unaligned_pointers": unaligned_pointers,
    }
    # Keep the default ABI usable with engines predating this opt-in key.
    if with_context:
        request_fields["with_context"] = True
    if observations is not None:
        if not isinstance(observations, Mapping):
            raise TypeError("observations must be a mapping")
        request_fields["observations"] = dict(observations)
    if source_contracts is not None:
        if not isinstance(source_contracts, Mapping):
            raise TypeError("source_contracts must be a mapping")
        request_fields["source_contracts"] = dict(source_contracts)
    request = _json_bytes(request_fields, "request")
    buffer = (ctypes.c_ubyte * len(code)).from_buffer_copy(code)
    host = api if api is not None else HostAPI()
    if decoder is None:
        text = host.owned_string(
            "neverd_bytecode_recover_json_v1", buffer, len(code), request, len(request)
        )
    else:
        errors: list[BaseException] = []

        def select(context: Any, data: Any, size: int, address: int,
                   reply: Any, reply_context: Any) -> None:
            payload = b'{"error":"Python instruction decoder failed"}'
            try:
                if not errors:
                    assert decoder is not None
                    instruction = decoder(ctypes.string_at(data, size), address)
                    if not isinstance(instruction, Mapping):
                        raise TypeError("decoder must return an instruction mapping")
                    payload = _json_bytes(dict(instruction), "decoder reply")
            except BaseException as error:
                errors.append(error)
            try:
                if reply(reply_context, payload, len(payload)) != 1 and not errors:
                    errors.append(NeverDError("native decoder rejected the reply"))
            except BaseException as error:
                errors.append(error)

        # Keep the trampoline and its closure alive until native recovery ends.
        callback = abi.BytecodeDecoderV1(select)
        try:
            text = host.owned_string(
                "neverd_bytecode_recover_decoder_json_v1", buffer, len(code),
                request, len(request), callback, None,
            )
        finally:
            if errors:
                raise errors[0]
    if text is None:
        raise NeverDError("bytecode recovery could not allocate its result")
    try:
        result = json.loads(text)
    except (ValueError, TypeError) as error:
        raise NeverDError("invalid bytecode recovery response") from error
    if (not isinstance(result, dict) or
            type(result.get("schemaVersion")) is not int or
            result.get("schemaVersion") != 1):
        raise NeverDError("unsupported bytecode recovery response")
    if result.get("ok") is not True:
        raise NeverDError(str(result.get("error", "bytecode recovery failed")))
    counts = ("functions", "blocks", "decoded_instructions", "decoded_bytes",
              "input_bytes")
    if any(type(result.get(key)) is not int or result[key] < 0 for key in counts):
        raise NeverDError("invalid bytecode recovery coverage")
    # An older engine may omit the field only for the default unary ABI.
    selected_context = result.get("with_context", False)
    if type(selected_context) is not bool or selected_context != with_context:
        raise NeverDError("invalid bytecode recovery context contract")
    evidence_counts = [result.get(key, 0 if observations is None else None)
                       for key in ("mapped_observations", "unmapped_observations")]
    if any(type(value) is not int or value < 0 for value in evidence_counts):
        raise NeverDError("invalid bytecode source evidence coverage")
    contract_counts = [result.get(key, 0 if source_contracts is None else None)
                       for key in ("reconstructed_calls", "unresolved_contract_calls", "guarded_contract_calls")]
    if any(type(value) is not int or value < 0 for value in contract_counts):
        raise NeverDError("invalid bytecode source contract coverage")
    if cast(int, contract_counts[2]) > cast(int, contract_counts[0]):
        raise NeverDError("invalid guarded bytecode call coverage")
    if (result.get("scope") not in ("cfg", "state-c") or
            not isinstance(result.get("source"), str) or
            result["input_bytes"] != len(code) or
            result["decoded_bytes"] > len(code) or
            result["scope"] != ("cfg" if output == "check" else "state-c") or
            (output == "check" and result["source"] != "")):
        raise NeverDError("invalid bytecode recovery source contract")
    return BytecodeRecoveryResult(
        result["functions"], result["blocks"], result["decoded_instructions"],
        result["decoded_bytes"], result["input_bytes"],
        cast(Literal["cfg", "state-c"], result["scope"]), result["source"],
        selected_context,
        cast(int, evidence_counts[0]), cast(int, evidence_counts[1]),
        cast(int, contract_counts[0]), cast(int, contract_counts[1]), cast(int, contract_counts[2]),
    )


__all__ = ["BytecodeRecoveryResult", "recover_bytecode", "recover_bytecode_with_decoder"]
