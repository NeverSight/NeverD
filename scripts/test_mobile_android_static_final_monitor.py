#!/usr/bin/env python3
"""Verify monitor lifetimes between static final assignments."""
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
 for name in ("neverd","android-jar","java-home","compiler-java-home"):p.add_argument("--"+name,type=Path,required=True)
 for name in ("d8","work-dir"):p.add_argument("--"+name,type=Path)
 p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-static-final-monitor-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"static_final_monitor";originals=w/"originals";harness=w/"harness";name="StaticFinalMonitorHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/(name+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(2,9):raise RuntimeError("Owned static-final-monitor definition inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded={"Lfixture/Trace;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Trace.class",(originals/"fixture/Trace.class").read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for mode in range(12):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,static-final-monitor=matched\n":raise RuntimeError("Frozen static-final-monitor observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  text=(fixture/"java/fixture/Core.java").read_text();inside="Trace.inside(17, TOKEN, DATA);";cleanup="Trace.cleanup(TOKEN);";finish="VALUE = Trace.finish(TOKEN);"
  wrong=[
   ("wrong-phase",text.replace(inside,"Trace.inside(18, TOKEN, DATA);"),0),
   ("lost-inside",text.replace(inside,";"),0),
   ("duplicate-inside",text.replace(inside,inside+inside),0),
   ("lost-cleanup",text.replace(cleanup,";"),0),
   ("duplicate-cleanup",text.replace(cleanup,cleanup+cleanup),0),
   ("early-final-store",text.replace("        synchronized (TOKEN) {","        "+finish+"\n        synchronized (TOKEN) {").replace("        "+finish+"\n    }","\n    }"),0),
   ("lost-finish",text.replace(finish,"VALUE = 0;"),0),
   ("lost-negative-zero",text.replace("-0.0f","0.0f"),0),
   ("wrong-cleanup-token",text.replace(cleanup,"Trace.cleanup(new Object());"),0),
   ("wrapped-inside-failure",text.replace(inside,"try { "+inside+" } catch (Throwable failure) { throw new IllegalStateException(failure); }"),4),
   ("swallowed-cleanup",text.replace(cleanup,"try { "+cleanup+" } catch (Throwable failure) {}"),6),
   ("swallowed-inside",text.replace(inside,"try { "+inside+" } catch (Throwable failure) {}"),4),
   ("duplicate-finish",text.replace(finish,"VALUE = Trace.finish(TOKEN) + Trace.finish(TOKEN);"),0),
   ("missing-monitor",text.replace("synchronized (TOKEN)","if (TOKEN != null)"),0),
   ("changed-monitor",text.replace("synchronized (TOKEN)","synchronized (new Object())"),0),
   ("cleanup-while-held",text.replace(inside,inside+" "+cleanup).replace("        "+cleanup+"\n",""),0)]
  for label,replacement,mode in wrong:
   if replacement==text:raise RuntimeError("Static-final-tail counterexample did not change the program")
   source=w/label/"Core.java";source.parent.mkdir();source.open("x").write(replacement);compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned static final monitor mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong static-final-monitor program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned static-final-monitor definition")
  disassembly=v.run([v.dexdump,"-d",dex],"sdk-code");ops=re.findall(r"\|[0-9a-f]+:\s+([a-z][\w/-]*)",disassembly)
  if ops.count("monitor-enter")!=1 or ops.count("monitor-exit")!=2 or "throw" not in ops:raise RuntimeError("Owned D8 must retain one monitor and both original releases")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Static-final-tail coverage or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Static-final-tail source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Static-final-tail metadata changed beyond reported helpers: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Static-final-tail Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Static final assignment order, cleanup, array or failure identity changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen static-final-monitor input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":36,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
