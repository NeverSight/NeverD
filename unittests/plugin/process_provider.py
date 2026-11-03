"""Protocol adversaries and descendant lifetime fixture, not an SDK example."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import time

from neverd_gui_plugin import read_frame, write_frame

mode = sys.argv[1]
output = sys.stdout.buffer
if mode == "startup-error":
    print("ModuleNotFoundError: missing_helper_fixture", file=sys.stderr, flush=True)
    raise SystemExit(1)
if mode == "oversized":
    output.write(struct.pack(">I", 1024 * 1024 + 1))
    output.flush()
    time.sleep(10)
    raise SystemExit(0)
if mode == "invalid-json":
    output.write(struct.pack(">I", 1) + b"{")
    output.flush()
    raise SystemExit(0)
write_frame(output, dict(type="hello"))
request = read_frame(sys.stdin.buffer)
identity = dict(task_id=request["task_id"])
if mode == "wrong-task":
    write_frame(output, dict(type="progress", task_id="wrong", progress=dict(label="wrong")))
elif mode == "unauthorized-query":
    write_frame(output, dict(type="query", **identity, id="1", operation="rename", payload={}))
elif mode == "missing-result":
    pass
elif mode == "bad-hash":
    path = Path(request["output_dir"]) / "output.txt"
    path.write_text("hello", encoding="utf-8")
    write_frame(output, dict(type="result", **identity, result=dict(views=[], artifacts=[dict(title="Bad hash", path="output.txt", sha256="0" * 64)])))
elif mode == "escaping-artifact":
    write_frame(output, dict(type="result", **identity, result=dict(views=[], artifacts=[dict(title="Escaping", path="../outside.txt", sha256="0" * 64)])))
elif mode == "invalid-progress":
    write_frame(output, dict(type="progress", **identity, progress=dict(label="bad", done=2, total=1)))
elif mode == "partial-frame":
    output.write(struct.pack(">I", 10) + b"{")
    output.flush()
elif mode == "result-then-crash":
    write_frame(output, dict(type="result", **identity, result=dict(views=[])))
    raise SystemExit(19)
elif mode == "deep-result":
    # A valid envelope whose unchecked function list nests far below the frame
    # limit; Python's encoder cannot produce it, so the frame is written raw.
    digest = '"' + "0" * 64 + '"'
    deep = "[" * 100000 + "]" * 100000
    body = ('{"protocol":1,"type":"result","task_id":' + json.dumps(request["task_id"])
            + ',"result":{"views":[],"artifacts":[{"title":"Deep","path":"deep.bin",'
            + '"sha256":' + digest + '}],"image_addition":{"artifact":0,"sample_sha256":'
            + digest + ',"description":{"schema":"neverd.analysis-section.v1",'
            + '"name":"deep","address":"0x1000","permissions":"r","functions":['
            + deep + ']}}}}').encode()
    output.write(struct.pack(">I", len(body)) + body)
    output.flush()
elif mode == "descendant":
    marker = str(Path(request["output_dir"]) / "descendant-survived.txt")
    program = "import time;from pathlib import Path;time.sleep(1);Path(" + repr(marker) + ").write_text('survived')"
    subprocess.Popen([sys.executable, "-c", program], stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    write_frame(output, dict(type="result", **identity, result=dict(views=[])))
else:
    raise RuntimeError(mode)
