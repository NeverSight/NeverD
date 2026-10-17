#!/usr/bin/env python3
"""Two analysts share a project through git: the worker takes files a pull
changed, keeps comments not saved yet on top of them, and orders rows as
the command line does; with the command line, git merges their concurrent
edits row by row and marks only a row both changed."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from transport_test import Client

# PUSH1 instructions two bytes apart: 0xe and 0x10 are both instructions,
# ordered differently as numbers and as text.
PROGRAM = "6001" * 12


def run(worker, cli=None):
    with tempfile.TemporaryDirectory(prefix="neverd-git-") as directory:
        root = Path(directory)
        env = dict(os.environ, GIT_AUTHOR_NAME="a", GIT_AUTHOR_EMAIL="a@a",
                   GIT_COMMITTER_NAME="a", GIT_COMMITTER_EMAIL="a@a",
                   GIT_CONFIG_GLOBAL=str(root / "gitconfig"),
                   GIT_CONFIG_NOSYSTEM="1")

        def git(cwd, *args, check=True):
            result = subprocess.run(["git", *args], cwd=cwd, capture_output=True,
                                    text=True, env=env, timeout=60)
            assert not check or result.returncode == 0, (args, result.stdout, result.stderr)
            return result

        git(root, "init", "-q", "--bare", "-b", "main", "origin.git")
        alice, bob = root / "alice", root / "bob"
        git(root, "clone", "-q", "origin.git", "alice")
        (alice / "a.evm").write_text(PROGRAM, encoding="ascii")
        # The project folder brings its own .gitignore and .gitattributes; a
        # clone names the merge driver once.
        if cli:
            setup = subprocess.run([cli, "setup-git", str(alice)], capture_output=True,
                                   text=True, env=env, timeout=60)
            assert setup.returncode == 0, setup
            driver = git(alice, "config", "merge.neverd.driver").stdout
            assert "merge-project %O %A %B" in driver, driver
        git(alice, "add", "-A")
        git(alice, "commit", "-q", "-m", "binary")
        git(alice, "push", "-q", "origin", "HEAD:main")
        git(root, "clone", "-q", "origin.git", "bob")
        if cli:
            assert subprocess.run([cli, "setup-git", str(bob)], capture_output=True,
                                  env=env, timeout=60).returncode == 0

        def edit(who, *operations):
            client = Client(worker)
            try:
                opened = client.call("open", {"path": str(who / "a.evm")})
                assert opened["status"] == "ok", opened
                replies = []
                for operation, payload in operations:
                    reply = client.call(operation, payload)
                    assert reply["status"] == "ok", (operation, reply)
                    replies.append(reply["payload"])
                return replies
            finally:
                client.close()

        def rows(who, suffix):
            path = who / "a.evm.neverd" / ("%s.json" % suffix)
            return json.loads(path.read_text()) if path.exists() else []

        def publish(who, message):
            git(who, "add", "-A")
            git(who, "commit", "-q", "-m", message)
            git(who, "push", "-q", "origin", "HEAD:main")

        # Rows by address as numbers, as the command line orders them.
        edit(alice, ("rename", {"address": "0x0", "name": "alice_entry"}),
             ("annotation_set", {"address": "0x10", "text": "sixteen"}),
             ("annotation_set", {"address": "0xe", "text": "fourteen"}),
             ("save", {}))
        assert [row["addr"] for row in rows(alice, "annotations")] == ["0xe", "0x10"]
        publish(alice, "alice")
        tracked = git(alice, "ls-files").stdout.split()
        assert "a.evm.neverd/annotations.json" in tracked, tracked
        assert "a.evm.neverd/.gitignore" in tracked, tracked
        assert not any(name.startswith("a.evm.neverd/local/") for name in tracked), tracked

        git(bob, "pull", "-q", "origin", "main")
        edit(bob, ("rename", {"address": "0x4", "name": "bob_four"}))
        publish(bob, "bob")

        # After a pull, the undo history Alice left describes older files:
        # the worker takes the files as they are and edits go on.
        git(alice, "pull", "-q", "origin", "main")
        client = Client(worker)
        try:
            assert client.call("open", {"path": str(alice / "a.evm")})["status"] == "ok"
            refused = client.call("rename", {"address": "0x6", "name": "too_early"})
            assert refused["error"]["code"] == "foreign_edits", refused
            synced = client.call("project_sync", {})
            assert synced["status"] == "ok" and synced["payload"]["changed"], synced
            assert client.call("rename", {"address": "0x6", "name": "alice_six"})["status"] == "ok"
            # Files this worker wrote itself are no change.
            assert client.call("project_sync", {})["payload"]["changed"] is False

            # A comment staged here stays on top of what a pull brings.
            assert client.call("annotation_set", {"address": "0x8",
                                                  "text": "staged"})["status"] == "ok"
            edit(bob, ("annotation_set", {"address": "0x2", "text": "bob two"}),
                 ("save", {}))
            publish(bob, "bob note")
            git(alice, "pull", "-q", "--no-rebase", "origin", "main")
            synced = client.call("project_sync", {})
            assert synced["payload"] == {"changed": True, "tables": True, "local_types": False,
                                         "comments_kept": 1, "dirty": True}, synced
            assert client.call("save", {})["status"] == "ok"
        finally:
            client.close()
        notes = {row["addr"]: row["text"] for row in rows(alice, "annotations")}
        assert notes == {"0x2": "bob two", "0x8": "staged", "0xe": "fourteen",
                         "0x10": "sixteen"}, notes
        names = {row["addr"]: row["renamed"] for row in rows(alice, "renames")}
        assert names == {"0x0": "alice_entry", "0x4": "bob_four", "0x6": "alice_six"}, names
        publish(alice, "alice again")

        # A comment staged here that a pull brings as it is needs no save.
        client = Client(worker)
        try:
            assert client.call("open", {"path": str(alice / "a.evm")})["status"] == "ok"
            assert client.call("annotation_set", {"address": "0x14",
                                                  "text": "same"})["status"] == "ok"
            git(bob, "pull", "-q", "origin", "main")
            edit(bob, ("project_sync", {}),
                 ("annotation_set", {"address": "0x14", "text": "same"}), ("save", {}))
            publish(bob, "bob same")
            git(alice, "pull", "-q", "origin", "main")
            synced = client.call("project_sync", {})
            assert synced["payload"] == {"changed": True, "tables": True, "local_types": False,
                                         "comments_kept": 0, "dirty": False}, synced
        finally:
            client.close()

        # A file git left with conflict markers is named, not taken.
        renames = alice / "a.evm.neverd/renames.json"
        kept = renames.read_text()
        renames.write_text("<<<<<<< ours\n[]\n=======\n[]\n>>>>>>> theirs\n")
        client = Client(worker)
        try:
            assert client.call("open", {"path": str(alice / "a.evm")})["status"] == "ok"
            conflict = client.call("project_sync", {})
            assert conflict["error"]["code"] == "merge_conflict", conflict
            assert "renames.json" in conflict["error"]["message"], conflict
        finally:
            client.close()
        renames.write_text(kept)
        git(alice, "checkout", "--", ".")

        if not cli:
            print("git workflow: pulls taken, staged comments kept, rows by address, "
                  "conflict markers named")
            return

        # The command line writes the same rows the workbench does.
        git(bob, "pull", "-q", "origin", "main")
        annotate = subprocess.run([cli, "annotate", str(bob / "a.evm"), "--add=0xa",
                                   "--text=ten"], capture_output=True, text=True, env=env,
                                  timeout=60)
        assert annotate.returncode == 0, annotate
        assert '"addr": "0xa"' in (bob / "a.evm.neverd/annotations.json").read_text()
        publish(bob, "bob cli")

        # Concurrent edits at different addresses merge without a conflict.
        git(alice, "pull", "-q", "origin", "main")
        edit(alice, ("project_sync", {}), ("rename", {"address": "0xc", "name": "alice_c"}),
             ("annotation_set", {"address": "0xc", "text": "alice c"}), ("save", {}))
        publish(alice, "alice c")
        edit(bob, ("project_sync", {}), ("rename", {"address": "0x12", "name": "bob_12"}),
             ("annotation_set", {"address": "0x12", "text": "bob 12"}), ("save", {}))
        git(bob, "add", "-A")
        git(bob, "commit", "-q", "-m", "bob 12")
        merged = git(bob, "pull", "-q", "--no-rebase", "origin", "main", check=False)
        assert merged.returncode == 0, (merged.stdout, merged.stderr,
                                        git(bob, "status", "--short").stdout)
        names = {row["addr"]: row["renamed"] for row in rows(bob, "renames")}
        assert names["0xc"] == "alice_c" and names["0x12"] == "bob_12", names
        notes = {row["addr"]: row["text"] for row in rows(bob, "annotations")}
        assert notes["0xc"] == "alice c" and notes["0x12"] == "bob 12", notes
        # As the workbench writes it: the worker takes it without rewriting.
        before = (bob / "a.evm.neverd/annotations.json").read_bytes()
        edit(bob, ("project_sync", {}))
        assert (bob / "a.evm.neverd/annotations.json").read_bytes() == before
        git(bob, "push", "-q", "origin", "HEAD:main")

        # One row both changed differently is the conflict, and only it.
        git(alice, "pull", "-q", "origin", "main")
        edit(alice, ("project_sync", {}), ("rename", {"address": "0x4", "name": "alice_four"}),
             ("rename", {"address": "0x14", "name": "alice_14"}))
        publish(alice, "alice four")
        edit(bob, ("rename", {"address": "0x4", "name": "bob_again"}))
        git(bob, "add", "-A")
        git(bob, "commit", "-q", "-m", "bob four")
        conflicted = git(bob, "pull", "-q", "--no-rebase", "origin", "main", check=False)
        assert conflicted.returncode != 0
        text = (bob / "a.evm.neverd/renames.json").read_text()
        assert text.count("<<<<<<< ours") == 1 and "alice_14" in text, text
        ours = text[text.index("<<<<<<<"):text.index("=======")]
        theirs = text[text.index("======="):text.index(">>>>>>>")]
        assert "bob_again" in ours and "alice_four" in theirs, text
        print("git workflow: pulls taken, staged comments kept, rows by address, "
              "row-wise merges, one conflicting row marked")


if __name__ == "__main__":
    if not shutil.which("git"):
        print("git is not on the PATH")
        sys.exit(77)
    run(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
