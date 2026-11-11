#!/usr/bin/env python3
"""Verify exception-only monitor releases retain locks, throws and method metadata."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,subprocess,tempfile,zipfile
from contextlib import nullcontext
from pathlib import Path
try:
 from scripts.mobile_real_apps_android import dex_header,parse_dexdump
 from scripts.test_mobile_android_annotation_values import compiler_inventory
 from scripts.test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
 from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
 from scripts.test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
except ModuleNotFoundError as error:
 if error.name!="scripts":raise
 from mobile_real_apps_android import dex_header,parse_dexdump
 from test_mobile_android_annotation_values import compiler_inventory
 from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
 from test_mobile_android_monitor_sparse_cleanup import compiler_facts
 from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ("neverd","android-jar","java-home"):p.add_argument("--"+name,type=Path,required=True)
 for name in ("d8","work-dir"):p.add_argument("--"+name,type=Path)
 p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-exception-only-monitor-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);compiler=v;fixture=FIXTURES/"exception_only_monitor";originals=w/"originals";harness=w/"harness";name="ExceptionOnlyMonitorHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/"ExceptionOnlyMonitorCaller.java",FIXTURES/"harness"/(name+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(3,5):raise RuntimeError("Owned exception-only-monitor definition inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded={"Lfixture/Probe;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in ("Lfixture/Probe;",):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(4):
    for kind in range(2):
     for mode in range(8):
      text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(kind),str(mode)],f"{label}-{route}-{kind}-{mode}")
      if text!="attempts=2,exception-only-monitor=matched\n":raise RuntimeError("Frozen exceptional monitor observer changed")
      out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted((fixture/"java").rglob("*.java"))}
  instance="""    public synchronized void fail() throws IOException {
        Trace.probe.observe(this);
        throw new IOException("owned monitor failure");
    }"""
  block="""        synchronized (lock) {
            Trace.probe.observe(lock);
            throw new IOException("owned monitor failure");
        }"""
  wrong=[
   ("lost-method-lock","Core.java","public synchronized void fail()","public void fail()",0,0),
   ("lost-method-modifier","Core.java",instance,instance.replace("synchronized void","void").replace("        Trace.probe.observe(this);","        synchronized (this) { Trace.probe.observe(this);").replace("    }","        }\n    }"),0,0),
   ("lost-static-lock","Core.java","synchronized (lock) {","if (true) {",0,1),
   ("wrong-static-lock","Core.java","synchronized (lock) {","synchronized (new Object()) {",0,1),
   ("early-static-release","Core.java",block,"        synchronized (lock) {}\n        Trace.probe.observe(lock);\n        throw new IOException(\"owned monitor failure\");",0,1),
   ("wrong-probe-argument","Core.java","Trace.probe.observe(this);","Trace.probe.observe(new Object());",0,0),
   ("lost-callback","Core.java","Trace.probe.observe(this);",";",0,0),
   ("repeated-callback","Core.java","Trace.probe.observe(this);","Trace.probe.observe(this); Trace.probe.observe(this);",0,0),
   ("wrapped-checked","Core.java","Trace.probe.observe(this);","try { Trace.probe.observe(this); } catch (IOException failure) { throw new IOException(failure); }",1,0),
   ("wrong-message","Core.java","\n        throw new IOException(\"owned monitor failure\");","\n        throw new IOException(\"different owned message\");",0,0),
   ("lost-throw","Core.java","\n        throw new IOException(\"owned monitor failure\");","\n        return;",0,0),
   ("added-static-modifier","Core.java","public static void failLock","public static synchronized void failLock",0,1)]
  for label,filename,anchor,replacement,mode,kind in wrong:
   text=texts[filename]
   if text.count(anchor)!=1:raise RuntimeError("Exceptional monitor mutation anchor changed: "+label)
   replacement=text.replace(anchor,replacement)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(replacement);compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"1",str(kind),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned exceptional monitor mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong exceptional monitor program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned exception-only-monitor definition")
  disassembly=v.run([v.dexdump,"-d",dex],"sdk-instructions")
  if len(re.findall(r"\bmonitor-enter\b",disassembly))!=2 or len(re.findall(r"\bmonitor-exit\b",disassembly))!=2:
   raise RuntimeError("Owned exceptional-only monitor D8 release shape changed")
  (w/"monitor-witness.json").open("x").write(json.dumps({"status":"verified","acquisitions":2,"physical_releases":2,"instance_and_static_always_throw":True,"dex_sha256":frozen_dex},indent=2)+"\n")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Exceptional monitor coverage or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Exceptional monitor source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Exceptional monitor metadata changed beyond reported helpers: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Exceptional monitor Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Exceptional monitor effects, casts, returned identity or failures changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen exception-only-monitor input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":64,"attempts_per_case":2,"wrong_programs":len(wrong),"single_exceptional_release_witness":"verified","dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
