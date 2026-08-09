# Security Policy

NeverD is a binary analysis and decompilation engine. We take reports of
defects in the **toolchain itself** seriously — especially issues that let
untrusted inputs compromise the machine running `neverd` or `libneverd`, or
that cause incorrect lifting, decompilation, or binary rewrite beyond what
the user asked for.

This policy does **not** cover misuse of NeverD to analyze malware, crack
software, or build offensive tooling; that is outside the scope of
coordinated disclosure here.

---

## Supported Versions

Security fixes are applied to the default branch (`dev`) and, when
practical, cherry-picked to the latest release tag. Pre-release or
unsupported branches may not receive patches.

| Version | Supported |
|---------|-----------|
| Latest release tag (`v*`) | Yes |
| `dev` | Yes |
| Older tags | Best effort only |

---

## Reporting a Vulnerability

**Please do not open a public GitHub issue for security-sensitive reports.**

### Preferred channel

Use [GitHub Private Vulnerability Reporting](https://github.com/NeverSight/NeverD/security/advisories/new)
for this repository. That keeps details confidential until a fix is ready.

If private reporting is unavailable, contact the maintainers through an
existing private channel you already use with the project (do not post
exploit details or proof-of-concept code in public issues, pull requests,
or discussions).

### What to include

A strong report helps us reproduce and fix faster:

1. **Summary** — what breaks, and the impact (e.g. heap overflow in the
   PE loader, mis-lifted bounds check, crash on malformed EVM bytecode).
2. **Affected component** — loader (PE / ELF / Mach-O / EVM), decoder,
   lifter, MedIR/HighIR, LLVM backend, patch/rewriter, CLI driver,
   `libneverd` C API, plugin ABI, etc.
3. **NeverD version or commit** — version banner from `neverd --help`, C API
   `neverd_version()`, or the git SHA (`neverd --version` prints LLVM only).
4. **Host platform** — OS, architecture, and how NeverD was built.
5. **Minimal reproduction** — smallest binary, hex input, flags, and command
   line (attach only what is needed).
6. **Proof of concept** — crash log, ASan/Valgrind output, disassembly or
   IR diff showing wrong semantics, or before/after hash of a corrupted patch.
7. **Suggested severity** (optional) — your view of exploitability.

### What we need from you

- Good-faith research: do not access systems or data you do not own.
- Give us reasonable time to investigate and ship a fix before public
  disclosure (see timeline below).
- Do not exploit issues against third parties.

---

## In Scope

Reports we treat as security issues include, but are not limited to:

| Area | Examples |
|------|----------|
| **Engine crashes on untrusted input** | Memory corruption, stack overflow, or use-after-free when loading, decoding, lifting, decompiling, or patching **malformed or adversarial** PE / ELF / Mach-O / EVM inputs. |
| **Incorrect semantics** | The lifter, decompiler, or patch pipeline emits IR, C, Solidity, or machine code that violates documented 1:1 semantics for **supported** instructions or opcodes, in a way that could plausibly mislead analysis or weaken memory safety without the user opting into unsafe behavior. |
| **Binary rewrite integrity** | `patch` or rewriter output that silently corrupts the target binary, breaks relocations, or changes runtime behavior beyond the requested edit. |
| **Path and file handling** | Directory traversal, arbitrary file read/write, or unsafe symlink behavior in the CLI, loader, or SDK when given attacker-controlled paths, response files, or output locations. |
| **SDK / plugin boundary** | Memory-safety bugs, buffer overflows, or lifetime errors in `libneverd` or the plugin ABI when called with valid API usage on untrusted session inputs. |
| **Supply chain / build** | Compromise of official release artifacts, reproducible-build breaks that hide tampering, or critical secrets embedded in distributed binaries. |
| **Bundled dependencies** | Vulnerabilities in third-party code **as shipped in NeverD releases**, when exploitable through normal `neverd` or SDK use. |

---

## Out of Scope

The following are generally **not** treated as security vulnerabilities:

- **Intended analysis capability** — lifting, decompiling, disassembling, or
  patching binaries the user supplies, including malware, game clients, or
  protected executables, when NeverD behaves as documented.
- **User-controlled malicious binaries** — decompiled or lifted output that
  describes attacker logic; analyzing hostile code is expected capability.
- **Quality gaps on unsupported input** — obfuscation, packed code, unknown
  opcodes, or formats outside the [architecture coverage
  matrix](docs/architecture.md#support-and-test-depth). Report these as
  regular bugs unless NeverD documented a guarantee it failed to meet.
- **Strict-mode failures** — `UnliftedInstruction` or other fail-loud errors
  for unsupported semantics; throwing instead of guessing is correct behavior.
- **Denial of service** via extremely large inputs without a plausible
  security impact (still welcome as regular bugs).
- **Issues in LLVM, Capstone, or Unicorn upstream** — please report those to
  the respective projects; we may still track NeverD-specific triggers or
  workarounds.
- **Social engineering, physical access, or third-party game / anti-cheat
  systems** — outside this engine's threat model.

When in doubt, report privately anyway; we will clarify scope in the reply.

---

## Response Timeline

We aim to:

| Stage | Target |
|-------|--------|
| Initial acknowledgement | Within **72 hours** |
| Triage and severity assessment | Within **7 days** |
| Fix or mitigation plan | Depends on complexity; critical issues prioritized |
| Coordinated disclosure | After a fix is available on `dev` and, when applicable, a release |

We may ask for more information or offer a draft advisory for your review
before publication.

---

## Disclosure Policy

- We prefer **coordinated disclosure**: work with us on a fix before public
  release of details.
- Credit will be given in release notes or the GitHub Security Advisory
  unless you prefer to remain anonymous.
- We do not pursue legal action against researchers who follow this policy
  in good faith.

---

## Safe Harbor

We support responsible security research on NeverD builds you own or have
permission to test. Research conducted in line with this policy — private
report, no harm to third parties, reasonable disclosure timing — will not
be treated as an attack on our infrastructure.

---

## Hardening Recommendations for Users

NeverD processes arbitrary binaries. Operators should:

- Treat **input binaries and patch scripts** like code execution: only
  analyze untrusted files in isolated environments (VM, container, CI
  sandbox).
- Verify release artifacts against tagged sources when reproducibility
  matters.
- Do not run `neverd` with elevated privileges unless required.
- Review lifted IR, decompiled output, and patched binaries before relying
  on them in sensitive workflows.

---

## Security Updates

Fixed vulnerabilities will be announced via GitHub Security Advisories and
noted in release notes for tagged releases. Watch the repository releases
or enable GitHub security notifications for updates.
