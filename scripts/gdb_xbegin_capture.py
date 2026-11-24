"""Collect a single stopped QEMU/KVM XBEGIN observation from GDB."""

from dataclasses import dataclass
import json
from pathlib import Path
import re


GATE_BYTES = 16
EXCEPTION_VECTORS = 32
UD_VECTOR = 6
FRAME_BYTES = 5 * 8
XBEGIN_BYTES = 6
X86_EXECUTION_BREAKPOINTS = 4
U64_MAX = (1 << 64) - 1


def _registers(text):
    """Use QEMU's synchronized register bank, including the cached CS rights."""
    cpu = re.findall(r"^CPU#(\d+)\s*$", text, re.MULTILINE)
    table = re.search(r"^IDT=\s*([\da-fA-F]+)\s+([\da-fA-F]+)", text, re.MULTILINE)
    fields = dict(re.findall(r"\b(RIP|RSP|RAX|RFL|EFER|CR4)=([\da-fA-F]+)\b", text))
    segment_rows = re.findall(r"^(CS|SS)\s*=([\da-fA-F]+)\s+[\da-fA-F]+\s+[\da-fA-F]+"
                              r"\s+([\da-fA-F]+)", text, re.MULTILINE)
    segments = {name: (int(selector, 16), int(flags, 16)) for name, selector, flags in segment_rows}
    if len(cpu) != 1 or not table or len(fields) != 6 or len(segments) != 2:
        raise ValueError("expected one QEMU x86-64 'info registers' bank with CS, SS and IDT")
    bank = {name.lower(): int(value, 16) for name, value in fields.items()}
    bank["eflags"] = bank.pop("rfl")
    bank.update(cpu=int(cpu[0]), cs=segments["CS"][0], ss=segments["SS"][0],
                cs_ar=segments["CS"][1] >> 8)
    return bank, tuple(int(word, 16) for word in table.groups())


def _gate_address(gate):
    return int.from_bytes(gate[:2] + gate[6:12], "little")


def _wire_bank(bank):
    return {key: value if key == "cpu" else hex(value) for key, value in bank.items()}


@dataclass
class XbeginObservation:
    before: dict[str, int]
    idtr: tuple[int, int]
    code: bytes
    idt: bytes
    fallthrough: int
    fallback: int
    handler: int
    stops: set[int]

    @classmethod
    def begin(cls, registers, read):
        bank, idtr = _registers(registers)
        if not (bank["efer"] & 0x400 and bank["cs_ar"] & 0x2000) or bank["cs"] & 3:
            raise ValueError("the stopped vCPU must be in 64-bit ring zero")
        if bank["eflags"] & 0x300:
            raise ValueError("IF and TF must be clear before observing XBEGIN")
        pc = bank["rip"]
        code = read(pc, XBEGIN_BYTES)
        if code[:2] != b"\xc7\xf8":
            raise ValueError("the stopped RIP must contain an unprefixed XBEGIN rel32")
        fallthrough = (pc + XBEGIN_BYTES) & U64_MAX
        fallback = (fallthrough + int.from_bytes(code[2:], "little", signed=True)) & U64_MAX
        size = EXCEPTION_VECTORS * GATE_BYTES
        if idtr[1] < size - 1:
            raise ValueError("the IDT limit does not cover the exception gates")
        idt = read(idtr[0], size)
        gates = [idt[pos:pos + GATE_BYTES] for pos in range(0, size, GATE_BYTES)]
        targets = {n: _gate_address(gate) for n, gate in enumerate(gates) if gate[5] & 0x80}
        handler = targets.get(UD_VECTOR)
        if handler is None or list(targets.values()).count(handler) != 1:
            raise ValueError("the isolated fixture needs a dedicated vector 6 landing address")
        handlers = set(targets.values())
        if handlers.intersection((fallthrough, fallback)):
            raise ValueError("an exception landing address aliases an XBEGIN successor")
        stops = handlers | {fallthrough, fallback}
        if pc in stops or len(stops) > X86_EXECUTION_BREAKPOINTS:
            raise ValueError("all exits must fit distinct hardware stops away from the initial RIP")
        return cls(bank, idtr, code, idt, fallthrough, fallback, handler, stops)

    def finish(self, registers, read, breakpoint_pc, load_base=None):
        after, idtr = _registers(registers)
        pc = after["rip"]
        if pc != breakpoint_pc:
            raise ValueError("the synchronized QEMU RIP disagrees with the GDB hardware breakpoint")
        if self.before["cpu"] != after["cpu"] or self.idtr != idtr:
            raise ValueError("the vCPU or IDTR changed between the two stops")
        if (read(self.before["rip"], XBEGIN_BYTES) != self.code or
                read(idtr[0], len(self.idt)) != self.idt):
            raise ValueError("the instruction or exception gates changed between stops")
        if pc == self.fallthrough == self.fallback:
            return None, f"XBEGIN successors coincide at {pc:#x}. Outcome undetermined"
        if pc == self.fallback:
            eax = after["rax"] & 0xffffffff
            return None, f"XBEGIN reached its abort target {pc:#x} with EAX={eax:#x} on this attempt"
        if pc == self.fallthrough:
            return None, f"XBEGIN reached its fallthrough at {pc:#x}"
        if pc != self.handler:
            return None, f"Stopped at {pc:#x}, outside the dedicated vector 6 entry"
        # The exporter authenticates the captured instruction, register fields,
        # gate and frame. Its V6 fault writer owns the delivered semantics.
        frame = read(after["rsp"], FRAME_BYTES)
        record = {"before": _wire_bank(self.before), "after": _wire_bank(after),
                  "bytes": self.code.hex(), "exception_idt": self.idt.hex(), "frame": frame.hex()}
        if load_base is not None:
            record["load_base"] = hex(load_base)
        return {"qemu_xbegin_fault_v1": record}, f"Captured the vector 6 landing at {pc:#x}"


def _install(gdb):
    class CaptureCommand(gdb.Command):
        """Arm hardware breakpoints for a single QEMU/KVM XBEGIN observation.

neverd-xbegin-capture FILE [LOAD_BASE] collects the first stop after continue.
neverd-xbegin-capture cancel removes the capture breakpoints."""

        def __init__(self):
            super().__init__("neverd-xbegin-capture", gdb.COMMAND_DATA, gdb.COMPLETE_FILENAME)
            self.breakpoints = {}
            self.pending = None

        def disarm(self, event=None):
            if self.pending is not None:
                gdb.events.stop.disconnect(self.stopped)
                gdb.events.exited.disconnect(self.disarm)
                gdb.events.connection_removed.disconnect(self.disarm)
                self.pending = None
            for bp in self.breakpoints:
                if bp.is_valid():
                    bp.delete()
            self.breakpoints.clear()

        @staticmethod
        def read_memory(inferior):
            # Bypass GDB's executable/stack cache and bind each read to guest virtual memory.
            connection = inferior.connection
            if connection.send_packet("qqemu.PhyMemMode") != b"0":
                raise ValueError("select QEMU virtual memory mode before capturing")

            def read(address, size):
                reply = connection.send_packet(f"m{address:x},{size:x}")
                if reply is None or not re.fullmatch(b"[0-9a-fA-F]{%d}" % (size * 2), reply):
                    raise ValueError(f"could not read {size} guest bytes at {address:#x}")
                return bytes.fromhex(reply.decode("ascii"))
            return read

        def stopped(self, event):
            observation, identity, path, load_base = self.pending
            try:
                inferior = gdb.selected_inferior()
                thread = gdb.selected_thread()
                hit = None
                if isinstance(event, gdb.BreakpointEvent):
                    hit = next((bp for bp in event.breakpoints if bp in self.breakpoints), None)
                if thread is None or (inferior.connection_num, thread.global_num) != identity or hit is None:
                    raise ValueError("the first stop was not a capture breakpoint on the original vCPU")
                registers = gdb.execute("monitor info registers", to_string=True)
                record, message = observation.finish(registers, self.read_memory(inferior),
                                                     self.breakpoints[hit], load_base)
                if record is not None:
                    with path.open("x", encoding="utf-8") as output:
                        output.write(json.dumps(record, indent=2) + "\n")
                    message += f" in {path}"
                gdb.write(message + "\n")
            except (gdb.error, OSError, ValueError) as error:
                gdb.write(f"XBEGIN capture: {error}\n", gdb.STDERR)
            finally:
                self.disarm()

        def invoke(self, argument, from_tty):
            self.dont_repeat()
            argv = gdb.string_to_argv(argument)
            if argv == ["cancel"]:
                self.disarm()
                return
            if len(argv) not in (1, 2):
                raise gdb.GdbError("usage: neverd-xbegin-capture FILE [LOAD_BASE]")
            if self.pending is not None:
                raise gdb.GdbError("a capture is pending. Continue or cancel it first")
            try:
                path = Path(argv[0]).expanduser().absolute()
                if not path.parent.is_dir():
                    raise ValueError(f"output directory does not exist: {path.parent}")
                if path.exists():
                    raise ValueError(f"output already exists: {path}")
                load_base = int(argv[1], 0) if len(argv) == 2 else None
                inferior = gdb.selected_inferior()
                thread = gdb.selected_thread()
                if thread is None or not thread.is_stopped() or len(inferior.threads()) != 1:
                    raise ValueError("select one stopped QEMU vCPU")
                if gdb.execute("monitor info kvm", to_string=True).strip() != "kvm support: enabled":
                    raise ValueError("the observation requires QEMU/KVM")
                cpu_list = gdb.execute("monitor info cpus", to_string=True)
                cpus = re.findall(r"^\s*\*?\s*CPU #(\d+):", cpu_list, re.MULTILINE)
                if len(cpus) != 1:
                    raise ValueError("use an isolated QEMU machine with one vCPU")
                regs = gdb.execute("monitor info registers", to_string=True)
                observation = XbeginObservation.begin(regs, self.read_memory(inferior))
                if observation.before["cpu"] != int(cpus[0]):
                    raise ValueError("the register bank does not match the QEMU vCPU")
                pc = observation.before["rip"]
                if load_base is not None and not 0 <= load_base <= pc:
                    raise ValueError("load base must be an unsigned address at or below the stopped RIP")
                for bp in gdb.breakpoints():
                    if (bp.enabled and bp.type in (gdb.BP_BREAKPOINT, gdb.BP_HARDWARE_BREAKPOINT) and
                            any(loc.enabled and loc.address == pc for loc in bp.locations)):
                        raise ValueError("disable the breakpoint at the current RIP to avoid a debugger step-over")
                for address in sorted(observation.stops):
                    bp = gdb.Breakpoint(f"*{address:#x}", type=gdb.BP_HARDWARE_BREAKPOINT, internal=True)
                    self.breakpoints[bp] = address
                    bp.inferior = inferior.num
                self.pending = observation, (inferior.connection_num, thread.global_num), path, load_base
                gdb.events.stop.connect(self.stopped)
                gdb.events.exited.connect(self.disarm)
                gdb.events.connection_removed.connect(self.disarm)
                gdb.write(f"Armed XBEGIN at {pc:#x}. Continue once to collect its first stop.\n")
            except (gdb.error, OSError, ValueError) as error:
                self.disarm()
                raise gdb.GdbError(str(error)) from error

    CaptureCommand()


if __name__ == "__main__":
    import gdb

    _install(gdb)
