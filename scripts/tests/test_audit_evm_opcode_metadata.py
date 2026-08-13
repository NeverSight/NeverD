import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts.audit_evm_opcode_metadata import (
    DEFAULT_GETH_CACHE,
    DEFAULT_GETH_REF,
    DEFAULT_GETH_REMOTE,
    DEFAULT_NEVERD_OPCODES,
    DEFAULT_POLICY,
    OpcodeAuditError,
    audit_opcodes,
    fetch_geth_opcode_source,
    parse_args,
    parse_geth_opcodes,
    parse_neverd_opcodes,
    parse_policy,
)


class EVMOpcodeAuditTests(unittest.TestCase):
    @staticmethod
    def _git(repository: Path, *arguments: str) -> str:
        result = subprocess.run(
            ("git", "-C", str(repository), *arguments),
            check=True,
            capture_output=True,
            encoding="utf-8",
        )
        return result.stdout.strip()

    def test_default_source_fetches_official_remote_head(self):
        arguments = parse_args([])
        self.assertIsNone(arguments.geth_root)
        self.assertEqual(
            arguments.geth_remote,
            "https://github.com/ethereum/go-ethereum.git",
        )
        self.assertEqual(arguments.geth_remote, DEFAULT_GETH_REMOTE)
        self.assertEqual(arguments.geth_ref, DEFAULT_GETH_REF)
        self.assertEqual(arguments.geth_cache, DEFAULT_GETH_CACHE)

    def test_default_neverd_metadata_paths_exist(self):
        self.assertTrue(DEFAULT_NEVERD_OPCODES.is_file())
        self.assertTrue(DEFAULT_POLICY.is_file())

    def test_fetches_and_refreshes_remote_head(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            upstream = root / "upstream"
            cache = root / "cache.git"
            upstream.mkdir()
            self._git(upstream, "init", "--initial-branch=master")
            self._git(upstream, "config", "user.name", "NeverD Test")
            self._git(upstream, "config", "user.email", "neverd@example.invalid")
            self._git(upstream, "config", "commit.gpgsign", "false")

            opcode_path = upstream / "core/vm/opcodes.go"
            opcode_path.parent.mkdir(parents=True)
            opcode_path.write_text(
                "const (\n    STOP OpCode = 0x00\n)\n", encoding="utf-8"
            )
            self._git(upstream, "add", "core/vm/opcodes.go")
            self._git(upstream, "commit", "-m", "add stop")
            first_revision = self._git(upstream, "rev-parse", "HEAD")

            first = fetch_geth_opcode_source(
                remote=str(upstream), ref="HEAD", cache=cache
            )
            self.assertEqual(first.revision, first_revision)
            self.assertIn("STOP OpCode", first.text)

            opcode_path.write_text(
                "const (\n    STOP OpCode = 0x00\n    ADD\n)\n",
                encoding="utf-8",
            )
            self._git(upstream, "add", "core/vm/opcodes.go")
            self._git(upstream, "commit", "-m", "add arithmetic")
            second_revision = self._git(upstream, "rev-parse", "HEAD")

            second = fetch_geth_opcode_source(
                remote=str(upstream), ref="HEAD", cache=cache
            )
            self.assertEqual(second.revision, second_revision)
            self.assertNotEqual(second.revision, first.revision)
            self.assertIn("ADD", second.text)

    def test_fetch_rejects_a_non_bare_cache(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            cache = Path(temporary_directory) / "cache.git"
            cache.mkdir()
            with self.assertRaisesRegex(OpcodeAuditError, "bare Git repository"):
                fetch_geth_opcode_source(
                    remote="unused", ref="HEAD", cache=cache
                )

    def test_fetch_rejects_ambiguous_or_option_like_refs(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            cache = Path(temporary_directory) / "cache.git"
            for ref in ("master", "--upload-pack=malicious", "refs/heads/a..b"):
                with self.subTest(ref=ref):
                    with self.assertRaisesRegex(OpcodeAuditError, "full refs"):
                        fetch_geth_opcode_source(
                            remote="unused", ref=ref, cache=cache
                        )

    def test_parses_go_iota_and_aliases(self):
        opcodes = parse_geth_opcodes(
            """
            const (
                PUSH1 OpCode = 0x60 + iota
                PUSH2
                PUSH3
            )
            const (
                DIFFICULTY OpCode = 0x44
                PREVRANDAO OpCode = 0x44
            )
            """
        )
        self.assertEqual(opcodes["PUSH1"], 0x60)
        self.assertEqual(opcodes["PUSH3"], 0x62)
        self.assertEqual(opcodes["PREVRANDAO"], 0x44)

    def test_alias_and_explicit_ignore_form_a_closed_inventory(self):
        neverd = parse_neverd_opcodes(
            """
            EVM_OPCODE(SHA3, 0x20, 2, 1)
            EVM_OPCODE(STOP, 0x00, 0, 0)
            """
        )
        upstream = parse_geth_opcodes(
            """
            const (
                STOP OpCode = 0x00
                KECCAK256 OpCode = 0x20
                EOFONLY OpCode = 0xe0
            )
            """
        )
        policy = parse_policy(
            """
            EVM_UPSTREAM_OPCODE_ALIAS(SHA3, KECCAK256)
            EVM_UPSTREAM_OPCODE_IGNORE(EOFONLY, WithdrawnEOF)
            """
        )
        result = audit_opcodes(neverd, upstream, policy)
        self.assertEqual(result.neverd_count, 2)
        self.assertEqual(result.ignored_count, 1)

    def test_byte_drift_fails_with_both_values(self):
        with self.assertRaisesRegex(OpcodeAuditError, "NeverD=0x01.*0x02"):
            audit_opcodes(
                {"ADD": 0x01},
                {"ADD": 0x02},
                parse_policy(""),
            )

    def test_unreviewed_upstream_opcode_fails(self):
        with self.assertRaisesRegex(OpcodeAuditError, "unreviewed.*NEWOP"):
            audit_opcodes(
                {"STOP": 0x00},
                {"STOP": 0x00, "NEWOP": 0x01},
                parse_policy(""),
            )

    def test_unsupported_upstream_expression_fails_closed(self):
        with self.assertRaisesRegex(OpcodeAuditError, "unsupported.*expression"):
            parse_geth_opcodes(
                """
                const (
                    STOP OpCode = byte(0x00)
                )
                """
            )

    def test_non_opcode_const_blocks_are_ignored(self):
        opcodes = parse_geth_opcodes(
            """
            const (
                WORD_BYTES = 32
            )
            const (
                STOP OpCode = 0x00
            )
            """
        )
        self.assertEqual(opcodes, {"STOP": 0x00})

    def test_duplicate_upstream_declaration_fails(self):
        with self.assertRaisesRegex(OpcodeAuditError, "duplicate.*STOP"):
            parse_geth_opcodes(
                """
                const (
                    STOP OpCode = 0x00
                    STOP OpCode = 0x00
                )
                """
            )

    def test_unparsed_standalone_opcode_declaration_fails_closed(self):
        with self.assertRaisesRegex(OpcodeAuditError, "unparsed.*NEWOP"):
            parse_geth_opcodes(
                """
                const (
                    STOP OpCode = 0x00
                )
                const NEWOP OpCode = 0x01
                """
            )


if __name__ == "__main__":
    unittest.main()
