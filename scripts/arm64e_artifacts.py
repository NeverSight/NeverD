"""Own disposable arm64e verifier products while preserving inputs and diagnostics."""

import fcntl
import hashlib
import json
from pathlib import Path
import shutil
import signal
import subprocess

POLICY = json.loads(Path(__file__).with_suffix(".def").read_text())


class OwnedProducts(list):
    """Journal disposable outputs before spawning their producer."""

    def __init__(self, root, keep, protected_inputs=()):
        super().__init__()
        self.root, self.keep = root, keep
        self.policy = POLICY
        self.protected_inputs = protected_inputs

    def relative(self, path):
        name = path.relative_to(self.root)
        if ".." in name.parts or name == Path():
            raise ValueError(self.policy["invalid_product"])
        return name.as_posix()

    def copy_input(self, source, target):
        if target.exists():
            if source.read_bytes() != target.read_bytes():
                raise ValueError(self.policy["changed_input"])
            return
        temporary = self.root / self.policy["pending_input"]
        shutil.copy2(source, temporary)
        temporary.replace(target)

    def save(self, records):
        temporary = self.root / self.policy["pending_manifest"]
        temporary.write_text(json.dumps({"retained": self.keep, "products": records,
                                         "preserved_products": self.preserved},
                                        indent=2) + "\n")
        temporary.replace(self.root / self.policy["manifest"])

    def append(self, path):
        name = self.relative(path)
        self.preserved.pop(name, None)
        super().append(path)
        self.save({self.relative(product): {} for product in self})

    def extend(self, paths):
        for path in paths:
            self.append(path)

    def run(self, *args, **kwargs):
        # A compiler that outlives a killed controller keeps the ownership lock
        # until it exits. A later controller cannot remove its pending output.
        return subprocess.run(*args, **kwargs, pass_fds=(self.lock.fileno(),))

    def __enter__(self):
        self.root.mkdir(parents=True, exist_ok=True)
        self.lock = (self.root / self.policy["lock"]).open("a+")
        fcntl.flock(self.lock, fcntl.LOCK_EX)
        try:
            manifest = self.root / self.policy["manifest"]
            previous = json.loads(manifest.read_text()) if manifest.exists() else {}
            self.preserved = previous.get("preserved_products", {})
            if previous.get("retained"):
                self.preserved.update(previous.get("products", {}))
            if not previous.get("retained"):
                for name in previous.get("products", {}):
                    if Path(name).is_absolute() or ".." in Path(name).parts:
                        raise ValueError(self.policy["invalid_product"])
                    if name not in self.protected_inputs:
                        (self.root / name).unlink(missing_ok=True)
            self.save({})
            self.previous_signal = signal.getsignal(signal.SIGTERM)

            def terminate(signum, frame):
                raise SystemExit(self.policy["terminated"])

            signal.signal(signal.SIGTERM, terminate)
            return self
        except BaseException:
            self.lock.close()
            raise

    def __exit__(self, kind, value, traceback):
        try:
            self.save({self.relative(path): {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                                   "bytes": path.stat().st_size}
                       for path in self if path.is_file()})
            if not self.keep:
                for path in self:
                    path.unlink(missing_ok=True)
        finally:
            signal.signal(signal.SIGTERM, self.previous_signal)
            self.lock.close()
