import unittest

from scripts.audit_evm_opcode_metadata import (
    OpcodeAuditError,
    audit_opcodes,
    parse_geth_opcodes,
    parse_neverd_opcodes,
    parse_policy,
)


class EVMOpcodeAuditTests(unittest.TestCase):
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
