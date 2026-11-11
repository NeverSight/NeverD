#!/usr/bin/env python3
"""Verify closed package constructors retain independent argument contracts."""
from __future__ import annotations
import re
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
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-long-constructor-continuation-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"long_constructor_continuation";originals=w/"originals";harness=w/"harness";name="fixture.LongConstructorContinuationHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/(name.rsplit(".",1)[-1]+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(3,10):raise RuntimeError("Owned long-constructor-continuation definition inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded=all_classes-{"Lfixture/Core;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in sorted(all_classes-{"Lfixture/Core;"}):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for mode in range(16):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,long-constructor-continuation=matched\n":raise RuntimeError("Frozen long-constructor-continuation observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted((fixture/"java").rglob("*.java"))}
  prefix="this(Provider.seed(provider));"
  middle="this.state = Trace.step(31, this, provider);"
  wrong=[
   ("lost-prefix","Core.java",prefix,"this((Object)null);",0),
   ("repeat-prefix","Core.java",prefix,"this(repeatSeed(provider));",0),
   ("wrong-final","Core.java","this.token = token;","this.token = null;",0),
   ("early-final","Core.java","Trace.hit(1, this, null, -1);\n        this.token = token;","this.token = token;\n        Trace.hit(1, this, null, -1);",0),
   ("lost-primary-observer","Core.java","Trace.hit(2, this, null, -1);",";",0),
   ("lost-suffix-entry","Core.java","Trace.hit(3, this, provider, -1);",";",0),
   ("lost-middle-step","Core.java",middle,";",0),
   ("wrong-middle-index","Core.java",middle,"this.state = Trace.step(32, this, provider);",0),
   ("repeat-middle-step","Core.java",middle,middle+middle,0),
   ("lost-middle-store","Core.java",middle,"Trace.step(31, this, provider);",0),
   ("early-tail-read","Core.java","this.tail = Provider.tail(provider);","this.tail = provider.tail;",1),
   ("wrong-suffix-branch","Core.java","if (chooseTail)","if (!chooseTail)",0),
   ("lost-terminal-observer","Core.java","Trace.hit(6, this, provider, -1);",";",0),
   ("wrapped-step-failure","Trace.java","Trace.hit(4, receiver, provider, index);","try { Trace.hit(4,receiver,provider,index); } catch(Throwable failure) { throw new IllegalStateException(failure); }",9)]
  for label,filename,anchor,replacement,mode in wrong:
   text=texts[filename]
   if text.count(anchor)!=1:raise RuntimeError("Constructor overload argument mutation anchor changed: "+label)
   replacement=text.replace(anchor,replacement)
   if label=="repeat-prefix":
    replacement=replacement[:-2]+"private static Object repeatSeed(Provider provider){Provider.seed(provider);return Provider.seed(provider); }\n}\n"
   source=w/label/filename;source.parent.mkdir();source.open("x").write(replacement);compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned long constructor continuation mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong long constructor continuation program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned long-constructor-continuation definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
  dump=v.run([v.dexdump,"-d",dex],"original-constructor-disassembly")
  witnesses=[]
  for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",dump):
   name_match=re.search(r"(?m)^\s+name\s+: '([^']+)'",block[2]);proto=re.search(r"(?m)^\s+type\s+: '([^']+)'",block[2])
   if block[1]!="Lfixture/Core;" or not name_match or name_match[1]!="<init>" or not proto or proto[1]!="(Lfixture/Provider;Z)V":continue
   ops=re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)",block[2]);delegates=[i for i,(op,ref) in enumerate(ops) if op.startswith("invoke-direct") and "Lfixture/Core;.<init>:" in ref]
   if len(delegates)!=1:raise RuntimeError("Original long constructor delegation changed")
   at=delegates[0];steps=[i for i,(op,ref) in enumerate(ops) if op.startswith("invoke-static") and "Lfixture/Trace;.step:" in ref]
   if len(ops)<=256 or at==0 or at>=257 or len(steps)!=72 or any(i<=at for i in steps):raise RuntimeError("Original long constructor continuation witness changed")
   witnesses.append({"operations":len(ops),"delegation":at,"suffix_steps":len(steps)})
  if len(witnesses)!=1:raise RuntimeError("Missing original long constructor continuation")
  (w/"original-constructor-witness.json").open("x").write(json.dumps(witnesses,indent=2)+"\n")
  output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=25:raise RuntimeError("Long-constructor-continuation coverage or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=25);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Long-constructor-continuation source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if (wanted["major"],actual["major"])!=(52,69):raise RuntimeError("Long-constructor-continuation class versions changed")
   actual["major"]=wanted["major"]
   if actual!=wanted:raise RuntimeError("Long-constructor-continuation metadata changed beyond reported helpers and required Java release: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Long-constructor-continuation Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Constructor overload argument effects, casts, returned identity or failures changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen long-constructor-continuation input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":25,"fresh_jvm_cases":48,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
