"""End-to-end Python binding coverage against the built libneverd."""

from __future__ import annotations

import ctypes
import os
from pathlib import Path
import unittest


class TranslationIntegrationTests(unittest.TestCase):
    def test_real_library_emits_aarch64_object_for_zero_flag_branch(self) -> None:
        library_path = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library_path:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        resolved_library = Path(library_path).resolve(strict=True)

        from neverd_plugin import TranslationObjectFormat
        from neverd_plugin import translate_x86_64_block_to_aarch64_object
        from neverd_plugin.ffi import HostAPI

        if hasattr(os, "add_dll_directory"):
            dll_directory = os.add_dll_directory(str(resolved_library.parent))
            self.addCleanup(dll_directory.close)
        translated = translate_x86_64_block_to_aarch64_object(
            b"\x74\xfe",
            entry_pc=0x401000,
            executable_generation=23,
            object_format=TranslationObjectFormat.ELF,
            host=HostAPI(ctypes.CDLL(str(resolved_library))),
        )

        self.assertGreater(len(translated.object_bytes), 20)
        self.assertEqual(translated.object_bytes[:4], b"\x7fELF")
        self.assertEqual(translated.object_bytes[18:20], b"\xb7\x00")  # EM_AARCH64
        self.assertEqual(translated.guest_entry_pc, 0x401000)
        self.assertEqual(translated.guest_instruction_count, 1)
        self.assertEqual(translated.guest_byte_count, 2)
        self.assertEqual(translated.executable_generation, 23)
        self.assertEqual(translated.host_triple, "aarch64-unknown-linux-gnu")
        self.assertTrue(translated.llvm_optimization_pipeline_ran)

    def test_real_library_emits_aarch64_object_for_single_flag_branch(self) -> None:
        library_path = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library_path:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        resolved_library = Path(library_path).resolve(strict=True)

        from neverd_plugin import TranslationObjectFormat
        from neverd_plugin import translate_x86_64_block_to_aarch64_object
        from neverd_plugin.ffi import HostAPI

        if hasattr(os, "add_dll_directory"):
            dll_directory = os.add_dll_directory(str(resolved_library.parent))
            self.addCleanup(dll_directory.close)
        translated = translate_x86_64_block_to_aarch64_object(
            b"\x0f\x8b\xfa\xff\xff\xff",
            entry_pc=0x401000,
            executable_generation=29,
            object_format=TranslationObjectFormat.ELF,
            host=HostAPI(ctypes.CDLL(str(resolved_library))),
        )

        self.assertGreater(len(translated.object_bytes), 20)
        self.assertEqual(translated.object_bytes[:4], b"\x7fELF")
        self.assertEqual(translated.object_bytes[18:20], b"\xb7\x00")
        self.assertEqual(translated.guest_entry_pc, 0x401000)
        self.assertEqual(translated.guest_instruction_count, 1)
        self.assertEqual(translated.guest_byte_count, 6)
        self.assertEqual(translated.executable_generation, 29)
        self.assertTrue(translated.llvm_optimization_pipeline_ran)

    def test_real_library_emits_aarch64_objects_for_multi_flag_branches(
        self,
    ) -> None:
        library_path = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library_path:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        resolved_library = Path(library_path).resolve(strict=True)

        from neverd_plugin import TranslationObjectFormat
        from neverd_plugin import translate_x86_64_block_to_aarch64_object
        from neverd_plugin.ffi import HostAPI

        if hasattr(os, "add_dll_directory"):
            dll_directory = os.add_dll_directory(str(resolved_library.parent))
            self.addCleanup(dll_directory.close)
        host = HostAPI(ctypes.CDLL(str(resolved_library)))
        encodings = (
            ("jbe", b"\x76\xfe"),
            ("ja", b"\x0f\x87\xfa\xff\xff\xff"),
            ("jl", b"\x7c\xfe"),
            ("jge", b"\x0f\x8d\xfa\xff\xff\xff"),
            ("jle", b"\x7e\xfe"),
            ("jg", b"\x0f\x8f\xfa\xff\xff\xff"),
        )
        for name, guest_bytes in encodings:
            with self.subTest(name=name):
                translated = translate_x86_64_block_to_aarch64_object(
                    guest_bytes,
                    entry_pc=0x401000,
                    executable_generation=31,
                    object_format=TranslationObjectFormat.ELF,
                    host=host,
                )

                self.assertGreater(len(translated.object_bytes), 20)
                self.assertEqual(translated.object_bytes[:4], b"\x7fELF")
                self.assertEqual(translated.object_bytes[18:20], b"\xb7\x00")
                self.assertEqual(translated.guest_entry_pc, 0x401000)
                self.assertEqual(translated.guest_instruction_count, 1)
                self.assertEqual(
                    translated.guest_byte_count,
                    len(guest_bytes),
                )
                self.assertEqual(translated.executable_generation, 31)
                self.assertTrue(translated.llvm_optimization_pipeline_ran)

    def test_real_library_emits_aarch64_objects_for_compare_and_test_branches(
        self,
    ) -> None:
        library_path = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library_path:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        resolved_library = Path(library_path).resolve(strict=True)

        from neverd_plugin import TranslationObjectFormat
        from neverd_plugin import translate_x86_64_block_to_aarch64_object
        from neverd_plugin.ffi import HostAPI

        if hasattr(os, "add_dll_directory"):
            dll_directory = os.add_dll_directory(str(resolved_library.parent))
            self.addCleanup(dll_directory.close)
        host = HostAPI(ctypes.CDLL(str(resolved_library)))
        encodings = (
            ("cmp-je", b"\x48\x39\xd8\x74\xfb"),
            ("test-jne", b"\x48\x85\xc0\x75\xfb"),
        )
        for name, guest_bytes in encodings:
            with self.subTest(name=name):
                translated = translate_x86_64_block_to_aarch64_object(
                    guest_bytes,
                    entry_pc=0x401000,
                    executable_generation=33,
                    object_format=TranslationObjectFormat.ELF,
                    host=host,
                )

                self.assertGreater(len(translated.object_bytes), 20)
                self.assertEqual(translated.object_bytes[:4], b"\x7fELF")
                self.assertEqual(translated.object_bytes[18:20], b"\xb7\x00")
                self.assertEqual(translated.guest_entry_pc, 0x401000)
                self.assertEqual(translated.guest_instruction_count, 2)
                self.assertEqual(translated.guest_byte_count, len(guest_bytes))
                self.assertEqual(translated.executable_generation, 33)
                self.assertTrue(translated.llvm_optimization_pipeline_ran)

    def test_real_library_emits_aarch64_objects_for_negative_compare_immediates(
        self,
    ) -> None:
        library_path = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library_path:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        resolved_library = Path(library_path).resolve(strict=True)

        from neverd_plugin import TranslationObjectFormat
        from neverd_plugin import translate_x86_64_block_to_aarch64_object
        from neverd_plugin.ffi import HostAPI

        if hasattr(os, "add_dll_directory"):
            dll_directory = os.add_dll_directory(str(resolved_library.parent))
            self.addCleanup(dll_directory.close)
        host = HostAPI(ctypes.CDLL(str(resolved_library)))
        encodings = (
            ("negative-imm8", b"\x48\x83\xf8\xfe\x74\xfa"),
            (
                "negative-imm32",
                b"\x48\x3d\xfe\xff\xff\xff\x74\xf8",
            ),
        )
        for name, guest_bytes in encodings:
            with self.subTest(name=name):
                translated = translate_x86_64_block_to_aarch64_object(
                    guest_bytes,
                    entry_pc=0x401000,
                    executable_generation=35,
                    object_format=TranslationObjectFormat.ELF,
                    host=host,
                )

                self.assertGreater(len(translated.object_bytes), 20)
                self.assertEqual(translated.object_bytes[:4], b"\x7fELF")
                self.assertEqual(translated.object_bytes[18:20], b"\xb7\x00")
                self.assertEqual(translated.guest_instruction_count, 2)
                self.assertEqual(translated.guest_byte_count, len(guest_bytes))
                self.assertTrue(translated.llvm_optimization_pipeline_ran)

    def test_real_library_rejects_reserved_test_opcode_extension(self) -> None:
        library_path = os.environ.get("NEVERD_TEST_LIBNEVERD")
        if not library_path:
            self.skipTest("NEVERD_TEST_LIBNEVERD is not configured")
        resolved_library = Path(library_path).resolve(strict=True)

        from neverd_plugin import TranslationError
        from neverd_plugin import TranslationErrorCode
        from neverd_plugin import TranslationObjectFormat
        from neverd_plugin import translate_x86_64_block_to_aarch64_object
        from neverd_plugin.ffi import HostAPI

        if hasattr(os, "add_dll_directory"):
            dll_directory = os.add_dll_directory(str(resolved_library.parent))
            self.addCleanup(dll_directory.close)
        host = HostAPI(ctypes.CDLL(str(resolved_library)))

        with self.assertRaises(TranslationError) as raised:
            translate_x86_64_block_to_aarch64_object(
                b"\x48\xf7\xc8\xff\xff\xff\xff\xc3",
                entry_pc=0x401000,
                executable_generation=37,
                object_format=TranslationObjectFormat.ELF,
                host=host,
            )

        self.assertIs(
            raised.exception.code,
            TranslationErrorCode.BLOCK_LOWERING_FAILED,
        )


if __name__ == "__main__":
    unittest.main()
