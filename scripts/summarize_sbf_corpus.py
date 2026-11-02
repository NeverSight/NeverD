"""Compare two pinned SBF qualification reports without losing failed rows."""

import argparse
import csv
import json
from pathlib import Path

try:
    from .run_sbf_corpus import STAGES
except ImportError:
    from run_sbf_corpus import STAGES


def comparable(before, after):
    for key in ("manifest_sha256", "expected_artifacts", "budgets"):
        if before["identity"][key] != after["identity"][key]:
            raise ValueError("comparison inputs differ: " + key)
    def inventory(report):
        return sorted((r["file"], r["sha256"], json.dumps(r["profile"], sort_keys=True))
                      for r in report["artifacts"])
    if inventory(before) != inventory(after):
        raise ValueError("artifact identities or runtime profiles differ")
    if [(r["file"], sorted(r["requests"])) for r in before["artifacts"]] != [
            (r["file"], sorted(r["requests"])) for r in after["artifacts"]]:
        raise ValueError("request scopes differ")
    expected = before["identity"]["expected_artifacts"]
    if len(before["artifacts"]) != expected or len(after["artifacts"]) != expected:
        raise ValueError("incomplete comparison inventory")


def state(row, kind, stage):
    return row.get("requests", {}).get(kind, {}).get("stages", {}).get(stage, {}).get("state", "not_run")


def reason(request, stage):
    details = request["stages"].get(stage, {}).get("details", {})
    message = details.get("error")
    if message and "output_bytes" in details:
        message += f" ({details['output_bytes']} bytes)"
    if message:
        return message
    if request.get("stop_reason"):
        return request["stop_reason"]
    if request.get("signal"):
        return f"signal {request['signal']}"
    error = request["stages"].get("analysis_error", {}).get("details", {}).get("error")
    return error or "see request stderr and stage events"


def make_report(before, after, before_path, after_path):
    comparable(before, after)
    count = after["identity"]["expected_artifacts"]
    budgets = after["identity"]["budgets"]
    lines = ["# SBF corpus 修复前后覆盖结果", "",
             f"固定输入：{count} 个；HEAD：`{after['identity']['head']}`。", "",
             f"修复前 worker SHA-256：`{before['identity']['worker_sha256']}`。", "",
             f"修复后 worker SHA-256：`{after['identity']['worker_sha256']}`。", "",
             "输入 SHA-256、runtime profile、请求超时、内存、输出预算及并发配置已比对一致。", "",
             f"预算：每请求 {budgets['timeout_seconds_per_request']} 秒；采样 RSS 上限 {budgets['sampled_rss_bytes_per_request']} bytes；输出上限 {budgets['output_bytes']} bytes；并发 {budgets['jobs']}。", "",
             "| 阶段 | 修复前通过 | 修复后通过 |", "| --- | ---: | ---: |"]
    phases = [(s, "analysis", s) for s in STAGES] + [(s + " 输出", s, s) for s in ("llvm", "c", "rust")]
    for label, kind, stage in phases:
        passed = [sum(state(r, kind, stage) == "passed" for r in report["artifacts"])
                  for report in (before, after)]
        lines.append(f"| {label} | {passed[0]}/{count} | {passed[1]}/{count} |")
    lines += ["", "阶段通过表示相应生产分析/输出过程完成。verification 为 requisite policy；独立官方 VM 验证不在本次扫描中。C/Rust 编译与实际程序执行未作为该表的通过条件。", "",
              "LLVM 阶段包含模块生成、verifyModule 和文本导出。输出大小超限发生在模块验证之后；超时或内存终止不能据此断言模块验证完成。", "",
              "## 修复后仍未通过的请求", "",
              "| 样本 | 请求 | 阶段 | 结果 | 具体原因 |", "| --- | --- | --- | --- | --- |"]
    failed = []
    for row in after["artifacts"]:
        if row["integrity"] != "passed":
            failed.append((row["name"], "integrity", "integrity", "failed", row.get("error", "")))
            continue
        for kind, request in row["requests"].items():
            active_failures = [(s, x) for s, x in request["stages"].items()
                               if x["state"] not in ("passed", "not_run") and s != "analysis_error"]
            for stage, value in active_failures:
                failed.append((row["name"], kind, stage, value["state"], reason(request, stage)))
            if not active_failures and request.get("process_state") != "passed":
                failed.append((row["name"], kind, "process", request["process_state"], reason(request, "process")))
            compilation = request.get("compilation", {})
            if compilation.get("state", "not_run") not in ("passed", "not_run"):
                failed.append((row["name"], kind, "compilation", compilation["state"],
                               compilation.get("stop_reason") or compilation.get("stderr_path", "compiler failed")))
    for fields in failed:
        lines.append("| " + " | ".join(str(x).replace("|", "\\|").replace("\n", " ") for x in fields) + " |")
    if not failed:
        lines.append("| 无 | | | | |")
    lines += ["", "## 全部样本", "",
              "| 样本 | 分析到 Solana | LLVM 输出 | C 输出 | Rust 输出 |", "| --- | --- | --- | --- | --- |"]
    for row in after["artifacts"]:
        states = [state(row, "analysis", "solana"), *[state(row, k, k) for k in ("llvm", "c", "rust")]]
        lines.append("| " + " | ".join([row["name"], *states]) + " |")
    lines += ["", "## 原始证据", "", f"- 修复前：{before_path}", f"- 修复后：{after_path}",
              "- 每个 request 保留准确命令、stage events、stderr、signal/预算状态和输出 SHA-256。", ""]
    return "\n".join(lines), failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    before = json.loads(args.before.read_text())
    after = json.loads(args.after.read_text())
    text, failed = make_report(before, after, args.before.resolve(), args.after.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)
    with args.output.with_suffix(".failures.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["artifact", "request", "stage", "state", "reason"])
        writer.writerows(failed)
    print(f"Wrote {args.output}; {len(failed)} failed stage requests retained")


if __name__ == "__main__":
    main()
