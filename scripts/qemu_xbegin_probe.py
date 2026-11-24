"""Observe one unresolved XBEGIN from a NeverD manifest on a fresh, diskless QEMU/KVM vCPU."""

import argparse
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile

from scripts import gdb_xbegin_capture as capture


PAGE = 4096
CS, SS = 0x300, 0x308
INITIAL_RAX = 0x1122334455667788
PROBE_TIMEOUT_SECONDS = 30


def _address(value):
    if isinstance(value, str):
        value = int(value, 0)
    if type(value) is not int or not 0 <= value <= capture.U64_MAX:
        raise ValueError("expected an unsigned x64 address")
    return value


def select_site(manifest, pc=None):
    if (not isinstance(manifest, dict) or manifest.get("schema_version") != 2 or
            manifest.get("profile") != "kernel-events-v6" or manifest.get("paged_memory") is not False or
            manifest.get("scheduled_events") is not False):
        raise ValueError("expected a successful-RAM kernel-events-v6 continuation manifest")
    if not isinstance(manifest.get("request"), dict) or not isinstance(manifest.get("units"), list):
        raise ValueError("the manifest must contain its request and instruction units")
    request = manifest["request"].copy()
    if (request.get("profile") != manifest["profile"] or request.get("paged_memory") or
            request.get("scheduled_events")):
        raise ValueError("the embedded request disagrees with the manifest's execution profile")
    if ("qemu_xbegin_fault_v1" in request or request.get("native_rtm_abort_regions") or
            request.get("native_rtm_regions")):
        raise ValueError("the request already has a competing RTM binding")
    selected = None
    for unit in manifest["units"]:
        if not isinstance(unit, dict):
            raise ValueError("invalid instruction unit in the manifest")
        if unit.get("native_boundary") is not True:
            continue
        address = _address(unit.get("pc"))
        if pc is not None and pc != address:
            continue
        raw = unit.get("bytes")
        if not isinstance(raw, str):
            raise ValueError("missing instruction bytes on a native unit")
        if len(raw) != 2 * capture.XBEGIN_BYTES:
            continue
        code = bytes.fromhex(raw)
        if capture.xbegin_successors(address, code) is None:
            continue
        if selected is not None:
            raise ValueError("multiple unresolved XBEGIN units match. Select a unique site with --pc")
        selected = address, code
    if selected is None:
        raise ValueError("no unresolved unprefixed XBEGIN rel32 matches the selection")
    if request.get("native_rtm_abort_steps", False) is not False:
        if request["native_rtm_abort_steps"] != "auto":
            raise ValueError("a declared or captured RTM binding belongs to its original CPU")
        # An automatic check at the sole requested instruction can use this new observation.
        instructions = request.get("instructions")
        if (not isinstance(instructions, list) or len(instructions) != 1 or len(manifest["units"]) != 1 or
                _address(instructions[0]) != selected[0]):
            raise ValueError("a multi-site RTM step binding cannot be replaced by one observed exception")
        del request["native_rtm_abort_steps"]
    return selected, request


def fixture_ram(pc, code, address_bits):
    """Map the six original bytes, their stops, a same-ring IDT and a fresh stack."""
    successors = capture.xbegin_successors(pc, code)
    if successors is None or address_bits not in (48, 57):
        raise ValueError("an XBEGIN rel32 and a 48- or 57-bit paging declaration are required")
    sites = (pc, pc + len(code) - 1, *successors)
    half = 1 << (address_bits - 1)
    if any(not (0 <= addr < half or (1 << 64) - half <= addr <= capture.U64_MAX) for addr in sites):
        raise ValueError("XBEGIN and its successors must be in canonical guest memory")
    if pc in successors:
        raise ValueError("a successor at the initial RIP would require a debugger step-over")

    page_mask = -PAGE
    code_pages = {addr & page_mask for addr in sites}
    # QEMU's reset GDTR and IDTR both point at zero. Reserve exactly the live records there.
    for lo, hi in ((0, capture.EXCEPTION_VECTORS * capture.GATE_BYTES), (CS, SS + 8)):
        if (pc < hi and pc + len(code) > lo) or any(lo <= addr < hi for addr in successors):
            raise ValueError("instruction or successor overlaps the fixture's descriptor tables")
    handler = 0x8000
    while handler in code_pages:
        handler += PAGE
    stack_page = 0x1d000
    while stack_page in code_pages or stack_page == handler:
        stack_page += PAGE

    # Physical page zero holds the reset descriptor tables. CR3 selects page one.
    ram = bytearray(2 * PAGE)
    physical = {}

    def allocate():
        start = len(ram)
        ram.extend(bytes(PAGE))
        return start

    for va in sorted(code_pages | {0, handler, stack_page}):
        pa = allocate() if va else 0
        physical[va] = pa
        if va in code_pages or va == handler:
            ram[pa:pa + PAGE] = b"\xf4" * PAGE
        table = PAGE
        for shift in range(address_bits - 9, 12, -9):
            slot = table + ((va >> shift) & 0x1ff) * 8
            entry, = struct.unpack_from("<Q", ram, slot)
            if not entry:
                entry = allocate() | 3
                struct.pack_into("<Q", ram, slot, entry)
            table = entry & page_mask
        struct.pack_into("<Q", ram, table + ((va >> 12) & 0x1ff) * 8, pa | 3)

    for vector in range(capture.EXCEPTION_VECTORS):
        destination = handler if vector == capture.UD_VECTOR else handler + 0x100
        struct.pack_into("<HHBBHII", ram, vector * capture.GATE_BYTES,
                         destination & 0xffff, CS, 0, 0x8e, (destination >> 16) & 0xffff,
                         destination >> 32, 0)
    struct.pack_into("<QQ", ram, CS, 0x00af9b000000ffff, 0x00cf93000000ffff)
    for offset, byte in enumerate(code):
        address = pc + offset
        ram[physical[address & page_mask] + (address % PAGE)] = byte
    return ram, stack_page + PAGE


def observe_in_gdb(pc, code, address_bits, rsp):
    """Called in a new batch GDB process with its working directory set to the fixture."""
    import gdb

    capture._install(gdb)
    gdb.execute("set architecture i386:x86-64", to_string=True)
    gdb.execute("target remote ./gdb.sock", to_string=True)
    try:
        if address_bits == 57:
            cpus = gdb.execute("monitor info hotpluggable-cpus", to_string=True)
            paths = re.findall(r'^\s*qom_path: "([^"]+)"', cpus, re.MULTILINE)
            if (len(paths) != 1 or
                    gdb.execute(f"monitor qom-get {paths[0]} la57", to_string=True).strip() != "true"):
                raise gdb.GdbError("the realized QEMU vCPU must expose LA57 for five-level paging")
        cr0 = int.from_bytes(gdb.selected_frame().read_register("cr0").bytes, "little")
        # Set the stopped register bank directly. No boot stub enters long mode.
        registers = {"cr4": 0x20 | (0x1000 if address_bits == 57 else 0), "cr3": PAGE,
                     "efer": 0x100, "cr0": (cr0 | 0x80000021) & ~0x60000000,
                     "cs": CS, "ss": SS, "rsp": rsp, "rax": INITIAL_RAX, "eflags": 2, "rip": pc}
        for name, value in registers.items():
            reg = gdb.selected_frame().read_register(name)
            reg.assign(gdb.Value(value.to_bytes(reg.type.sizeof, "little"), reg.type))
        bank, _ = capture._registers(gdb.execute("monitor info registers", to_string=True))
        if any(bank[key] != registers[key] for key in ("rip", "rsp", "cs", "ss", "cr4", "rax", "eflags")):
            raise gdb.GdbError("the vCPU did not enter the requested fixture state")
        connection = gdb.selected_inferior().connection
        if connection.send_packet(f"m{pc:x},{len(code):x}") != code.hex().encode("ascii"):
            raise gdb.GdbError("guest virtual instruction bytes differ from the selected manifest unit")
        gdb.execute("neverd-xbegin-capture fault.json", to_string=True)
        stop = gdb.execute("continue", to_string=True)
        if not Path("fault.json").is_file():
            gdb.write(stop)
    finally:
        # Request shutdown while stopped. QEMU may close the remote link before replying.
        try:
            gdb.execute("monitor quit", to_string=True)
        except gdb.error:
            if gdb.selected_inferior().connection is not None:
                raise


def collect_fault(site, code, bits):
    ram, rsp = fixture_ram(site, code, bits)
    with tempfile.TemporaryDirectory(prefix="neverd-xbegin-") as directory:
        work = Path(directory)
        (work / "fixture.ram").write_bytes(ram)
        root = str(Path(__file__).resolve().parents[1])
        entry = (f"python import sys; sys.path.insert(0, {root!r}); "
                 f"from scripts.qemu_xbegin_probe import observe_in_gdb; "
                 f"observe_in_gdb({site}, {code!r}, {bits}, {rsp})")
        gdb_command = ["gdb", "-nx", "-q", "-batch", "-iex", "set auto-load off", "-ex", entry]
        # QEMU inherits a listening socket. The debugger can connect without a startup poll.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(str(work / "gdb.sock"))
            listener.listen(1)
            fd = listener.fileno()
            qemu = ["qemu-system-x86_64", "-machine", "q35,accel=kvm", "-cpu", "host",
                    "-smp", "1", "-m", "32M", "-nodefaults", "-no-user-config", "-no-reboot",
                    "-display", "none", "-nic", "none", "-serial", "none", "-monitor", "none",
                    "-S", "-device", "loader,addr=0,file=fixture.ram,force-raw=on",
                    "-chardev", f"socket,id=debug,fd={fd},server=on,wait=off", "-gdb", "chardev:debug"]
            with subprocess.Popen(qemu, cwd=work, stdin=subprocess.DEVNULL, pass_fds=(fd,)) as vm:
                listener.close()
                try:
                    with subprocess.Popen(gdb_command, cwd=work, stdin=subprocess.DEVNULL) as debugger:
                        try:
                            status = debugger.wait(timeout=PROBE_TIMEOUT_SECONDS)
                        finally:
                            # Stop the guest first on timeout or interruption.
                            if debugger.poll() is None:
                                vm.kill()
                                debugger.kill()
                finally:
                    vm.kill()
        if status:
            raise ValueError("GDB could not complete the isolated XBEGIN observation")
        fragment = work / "fault.json"
        if not fragment.is_file():
            raise ValueError("the stop yielded no reusable vector 6 observation")
        return json.loads(fragment.read_text())["qemu_xbegin_fault_v1"]


def probe(manifest, output, pc=None):
    (site, code), request = select_site(manifest, pc)
    if output.exists():
        raise ValueError(f"output already exists: {output}")
    if not output.parent.is_dir():
        raise ValueError(f"output directory does not exist: {output.parent}")
    request["qemu_xbegin_fault_v1"] = collect_fault(site, code, _address(request.get("paging_address_bits")))
    # Publish only a complete request and leave an existing output intact on a concurrent run.
    with tempfile.NamedTemporaryFile("w", dir=output.parent, encoding="utf-8") as temporary:
        json.dump(request, temporary, indent=2)
        temporary.write("\n")
        temporary.flush()
        os.link(temporary.name, output)
    return site


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="unbound V6 continuation report")
    parser.add_argument("output", type=Path, help="new bound request for --x64-continuation")
    parser.add_argument("--pc", type=_address, help="image PC when multiple unresolved XBEGIN sites exist")
    args = parser.parse_args(argv)
    try:
        site = probe(json.loads(args.manifest.read_text()), args.output, args.pc)
        print(f"Recorded the vector 6 stop for XBEGIN at {site:#x} in {args.output}")
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"qemu-xbegin-probe: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
