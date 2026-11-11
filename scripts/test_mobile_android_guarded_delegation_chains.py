#!/usr/bin/env python3
"""Verify genuine Kotlin guarded constructor chains and primary final writes."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile


class GuardedClassFile(KotlinFixtureClassFile):
    SCHEMAS = {**KotlinFixtureClassFile.SCHEMAS, "Lkotlin/jvm/JvmOverloads;": {}}


def compiler_facts(root: Path) -> dict:
    result = {}
    for path in sorted(root.rglob("*.class")):
        row = GuardedClassFile(path.read_bytes()).facts()
        if row["name"] in result or path.relative_to(root).as_posix() != row["name"][1:-1] + ".class":
            raise RuntimeError("Duplicate/misplaced original compiler class")
        result[row["name"]] = row
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--kotlin-java-home", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    parser.add_argument("--guarded-primary", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-guarded-delegation-chains-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "guarded_delegation_chains"
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        kotlin = args.kotlin_lib.resolve()
        compiler_jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file() or not (kotlin / "kotlin-stdlib.jar").is_file():
            raise RuntimeError("The original witness requires the unmodified Kotlin compiler distribution")
        inputs = [args.android_jar.resolve(), *compiler_jars]
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        stdlib = kotlin / "kotlin-stdlib.jar"
        dependencies = []
        for jar in (stdlib, kotlin / "annotations-13.0.jar"):
            output = work / jar.name
            with zipfile.ZipFile(jar) as source, zipfile.ZipFile(output, "x") as dest:
                for entry in source.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        dest.writestr(entry.filename, source.read(entry))
            dependencies.append(output)
        observers=[FIXTURES / "harness" / (name + ".java") for name in ("GuardedDelegationHarness", "GuardedDelegationCaller")]
        if args.guarded_primary:
            copied=work/"guarded-primary-fixture";shutil.copytree(fixture,copied);fixture=copied
            core=fixture/"kotlin/fixture/Core.kt";text=core.read_text()
            before="Base(token, count, wide, number)"
            if text.count(before)!=1:raise RuntimeError("Original guarded primary super anchor changed")
            core.write_text(text.replace(before,"Base(if (Trace.gate()) Trace.choose(token) else null, count, wide, number)"))
            trace=fixture/"java/fixture/Trace.java";text=trace.read_text()
            text=text.replace("public final class Trace {","public final class Trace {\n    public static boolean flag;\n    public static Object chosen,constructing;\n    public static java.util.function.IntConsumer observer;")
            text=text.replace("events = events * 10 + value;","events = events * 10 + value;\n        if(observer!=null)observer.accept(value);")
            text=text.rsplit("}",1)[0]+"    public static boolean gate() { event(6); return flag; }\n    public static Object choose(Object token) { event(7); return chosen; }\n}\n"
            trace.write_text(text)
            base=fixture/"java/fixture/Base.java";text=base.read_text()
            if text.count("        Trace.event(4);")!=1:raise RuntimeError("Original superclass publication anchor changed")
            base.write_text(text.replace("        Trace.event(4);","        Trace.constructing=this;\n        Trace.event(4);"))
            frozen=[]
            for original in observers:
                text=original.read_text()
                if original.name.endswith("Harness.java"):
                    before="Trace.events = 0; Trace.observed = null;"
                    if text.count(before)!=1:raise RuntimeError("Original guarded primary observer setup changed")
                    text=text.replace(before,before+"\n            Trace.flag=variant==1;Trace.chosen=new Object();Trace.constructing=null;\n            Trace.observer=event->{\n                if(event==6||event==7)require(Trace.constructing==null&&Trace.observed==null);\n                if(event==4){Core partial=(Core)Trace.constructing;require(partial.stamp==0&&partial.token==null&&partial.count==0&&partial.wide==0&&Double.doubleToRawLongBits(partial.number)==0);}\n                if(event==5){Core partial=(Core)Trace.constructing;require(partial.stamp==0&&Trace.observed==partial&&partial.token==(Trace.flag?Trace.chosen:null));}\n            };")
                    before="""                if (failed == 0 && token != null) {
                    events = events * 10 + 4;
                    if (mode == 4) failed = 4;
                    else { events = events * 10 + 5; if (mode == 5) failed = 5; }
                }"""
                    after="""                if (failed == 0 && token != null) {
                    for(int event=6;event<=7;event++) {
                        if(event==7&&!Trace.flag)continue;
                        events=events*10+event;
                        if(mode==event){failed=event;break;}
                    }
                    if(failed==0){events=events*10+4;if(mode==4)failed=4;else{events=events*10+5;if(mode==5)failed=5;}}
                }"""
                    if text.count(before)!=1:raise RuntimeError("Original guarded primary event observer changed")
                    text=text.replace(before,after).replace("result.token == token", "result.token == (Trace.flag?Trace.chosen:null)")
                target=work/original.name;target.open("x").write(text);frozen.append(target)
            observers=frozen
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), original_java, release=8)
        originals.mkdir()
        kotlin_jdk = choose_jdk(args.kotlin_java_home)
        verify.run([kotlin_jdk / "bin" / ("java.exe" if os.name == "nt" else "java"), "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-jdk-home", kotlin_jdk,
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8", "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            destination = originals / path.relative_to(original_java)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        verify.compile_local(observers, harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (3, 13 if args.guarded_primary else 11):
            raise RuntimeError("Owned guarded delegation inventory changed: " + repr((len(all_classes), len(all_methods))))
        classes = all_classes - ({"Lfixture/Base;"} if args.dependency_parent else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        dependency = work / "owned-parent.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("fixture/Base.class", (originals / "fixture/Base.class").read_bytes())
        if args.dependency_parent:
            dependencies.append(dependency)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]
        synthetic = {identity for identity, row in records["Lfixture/Core;"]["methods"].items() if row["access"] & 0x1000}
        default = "Lfixture/Core;-><init>(Ljava/lang/Object;IJDILkotlin/jvm/internal/DefaultConstructorMarker;)V"
        if synthetic != {default} or records["Lfixture/Core;"]["methods"][default]["access"] != 0x1001:
            raise RuntimeError("Original Kotlin default constructor flags changed")

        def execute(runtime, label, original):
            cases = [(level, route, -1) for level in range(4) for route in range(4)]
            cases += [(0, route, bits * 2 + 1) for bits in range(8) for route in (2, 3)]
            result = []
            for level, route, mask in cases:
                for mode in range(8 if args.guarded_primary else 6):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                        "GuardedDelegationHarness", str(level), str(route), str(mask), str(mode), str(original).lower()],
                        f"{label}-{level}-{route}-{mask}-{mode}")
                    if output != f"level={level},route={route},mask={mask},mode={mode}:matched\n":
                        raise RuntimeError("Original guarded delegation matrix changed: " + output)
                    result.append(output)
            return result

        baseline = execute((originals, harness), "baseline", True)
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned guarded delegation DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original guarded constructor runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-delegation-disassembly")
        witnesses = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            kind = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not kind:
                continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
            if kind[1] == "(Ljava/lang/Object;)V":
                if ops[:2] != ["const-string", "invoke-static"] or ops[-2:] != ["invoke-direct/range", "return-void"]:
                    raise RuntimeError("Kotlin/D8 changed the checked this-chain witness")
                witnesses.add("checked-overload")
            if kind[1] == "(Ljava/lang/Object;IJD)V":
                if args.guarded_primary:
                    if not any(op.startswith("if-") for op in ops) or ops[-1]!="return-void" or "iput" not in ops:
                        raise RuntimeError("Kotlin/D8 changed the guarded primary witness")
                elif ops[:3] != ["const-string", "invoke-static", "invoke-direct/range"]:
                    raise RuntimeError("Kotlin/D8 changed the checked primary witness")
                witnesses.add("checked-primary")
            if kind[1] == default.split("<init>", 1)[1]:
                if ops.count("if-eqz") != 3 or ops.count("and-int/lit8") != 3 or ops[-2:] != ["invoke-direct/range", "return-void"]:
                    raise RuntimeError("Kotlin/D8 changed the guarded default witness")
                witnesses.add("guarded-default")
        if witnesses != {"checked-overload", "checked-primary", "guarded-default"}:
            raise RuntimeError("Missing an original guarded constructor witness")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        if "VISIBILITY_BUILD" in annotations or re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations) != ["Lkotlin/Metadata;"]:
            raise RuntimeError("D8 changed Kotlin annotation retention")
        # D8 omits these original CLASS-retained compiler annotations; they
        # are absent from the actual recovery input, independently checked above.
        def dex_metadata(value):
            if isinstance(value, dict):
                return {k: dex_metadata(v) for k, v in value.items() if k not in
                        {"runtime_invisible_annotations", "runtime_invisible_parameter_annotations"}}
            if isinstance(value, list):
                return [dex_metadata(v) for v in value]
            return value
        output = work / "recovered"
        # Complete Kotlin dependency declarations exceed the default fixed work
        # allowance. Keep a bounded request aligned with this profile's deadline.
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath,
                    "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {key for key, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or projected != {default} or coverage.get("projected_method_count") != 1
                or coverage["status"] != "partial" or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release") != 25 or rows[default].get("projection_kind") != "synthetic-constructor"
                or rows[default].get("source_identity") != default or len(rows[default].get("metadata_differences", [])) != 1):
            raise RuntimeError("Guarded delegation changed inventory or omitted synthetic flag disclosure")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage["generated_source_helpers"]}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Guarded delegation invented a runtime entry or omitted helper accounting")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            original, actual = dex_metadata(records[owner]), dex_metadata(after[owner])
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if original.get(key) != actual.get(key):
                    raise RuntimeError("Original guarded constructor class metadata changed: " + owner + ":" + key)
            for method, metadata in original["methods"].items():
                wanted = copy.deepcopy(metadata)
                if method == default:
                    wanted["access"] &= ~0x1000
                if actual["methods"].get(method) != wanted or after_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original guarded constructor metadata changed: " + method)
        if execute((rebuilt, harness), "rebuilt", False) != baseline:
            raise RuntimeError("Guarded constructor changed original values, identities or failures")
        wrong = work / "wrong-mask/fixture/Core.java"
        wrong.parent.mkdir(parents=True)
        wrong.write_text('''package fixture;
public final class Core extends Base {
 public final int stamp;
 public Core(Object token, int count, long wide, double number) { super(token, count, wide, number); stamp = Trace.event(5); }
 public Core(Object token, int count, long wide, double number, int mask, kotlin.jvm.internal.DefaultConstructorMarker marker) {
  this(token, (mask & 4) != 0 ? Trace.count() : count, (mask & 4) != 0 ? Trace.wide() : wide, (mask & 8) != 0 ? Trace.number() : number);
 }
 public Core(Object token, int count, long wide) { this(token, count, wide, Trace.number()); }
 public Core(Object token, int count) { this(token, count, Trace.wide()); }
 public Core(Object token) { this(token, Trace.count()); }
}
''')
        if args.guarded_primary:
            text=wrong.read_text();text=text.replace("super(token, count, wide, number)","super(Trace.gate()?Trace.choose(token):null, count, wide, number)");wrong.write_text(text)
        verify.compile_local([wrong], work / "wrong-mask-classes", classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (work / "wrong-mask-classes", originals, harness, *dependencies))),
            "GuardedDelegationHarness", "0", "2", "3", "0", "false"], text=True, capture_output=True, timeout=300, env=verify.environment)
        (work / "wrong-mask-counterexample.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or "AssertionError" not in result.stderr:
            raise RuntimeError("Independent observer did not detect the changed default mask")
        primary_counterexamples = []
        if args.guarded_primary:
            # Author a separate correct Java control. Its null check precedes
            # the guard just as in the original Kotlin program. The mutations
            # change only the behavior the frozen observer is meant to catch.
            control = wrong.read_text().replace("(mask & 4) != 0 ? Trace.count()", "(mask & 2) != 0 ? Trace.count()")
            control = control.replace("super(Trace.gate()?Trace.choose(token):null, count, wide, number)",
                                      "super(select(token), count, wide, number)")
            control = control.replace(" public final int stamp;", """ public final int stamp;
 private static Object select(Object token) {
  java.util.Objects.requireNonNull(token);
  return Trace.gate()?Trace.choose(token):null;
 }""")
            variants = {
                "primary-control": control,
                "reversed-primary-branch": control.replace("return Trace.gate()?", "return !Trace.gate()?"),
                "duplicated-primary-guard": control.replace("return Trace.gate()?", "return (Trace.gate() && Trace.gate())?"),
                "late-primary-evaluation": control.replace("stamp = Trace.event(5);", "Trace.gate(); stamp = Trace.event(5);"),
            }
            for label, program in variants.items():
                candidate = work / label / "fixture/Core.java"
                candidate.parent.mkdir(parents=True)
                candidate.open("x").write(program)
                candidate_classes = work / (label + "-classes")
                verify.compile_local([candidate], candidate_classes,
                                     classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
                result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (
                    candidate_classes, originals, harness, *dependencies))),
                    "GuardedDelegationHarness", "0", "0", "-1", "0", "false"],
                    text=True, capture_output=True, timeout=300, env=verify.environment)
                (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
                if label == "primary-control":
                    if result.returncode or result.stdout != "level=0,route=0,mask=-1,mode=0:matched\n":
                        raise RuntimeError("Independent guarded primary control failed")
                elif result.returncode == 0 or "AssertionError" not in result.stderr:
                    raise RuntimeError("Frozen observer missed " + label)
                else:
                    primary_counterexamples.append(label)
            # An independently changed superclass exposes its field stores
            # before the original publication callback.
            label = "early-parent-stores"
            candidate = work / label / "fixture/Base.java"
            candidate.parent.mkdir(parents=True)
            candidate.open("x").write('''package fixture;
public class Base {
 public final Object token; public final int count; public final long wide; public final double number;
 public Base(Object token,int count,long wide,double number) {
  Trace.constructing=this;
  this.token=token;this.count=count;this.wide=wide;this.number=number;
  Trace.event(4);Trace.observed=this;
 }
}
''')
            candidate_classes = work / (label + "-classes")
            verify.compile_local([candidate], candidate_classes,
                                 classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (
                candidate_classes, work / "primary-control-classes", originals, harness, *dependencies))),
                "GuardedDelegationHarness", "0", "0", "-1", "0", "false"],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "AssertionError" not in result.stderr:
                raise RuntimeError("Frozen observer missed early parent stores")
            primary_counterexamples.append(label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != h for p, h in input_hashes.items())
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != h for p, h in dependency_hashes.items())):
            raise RuntimeError("Original inputs, compiler, immutable callers or dependencies changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "dependency_parent": args.dependency_parent,"guarded_primary":args.guarded_primary,
                    "original_dex_metadata_and_exceptions": "unchanged except disclosed synthetic flag",
                    "default_mask_wide_bits_identity_and_null_order": "matched", "original_exception_identity": "matched",
                    "original_compiler_branches": "independently witnessed", "coverage": "partial", "minimum_java_release": 25,
                    "independent_primary_counterexamples": primary_counterexamples,
                    "independent_changed_mask_counterexample": "detected", "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
