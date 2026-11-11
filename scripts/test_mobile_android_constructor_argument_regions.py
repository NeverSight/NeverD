#!/usr/bin/env python3
"""Verify conditional argument allocations before an exact constructor delegation."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,subprocess,tempfile,zipfile
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
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-constructor-argument-regions-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"constructor_argument_regions";originals=w/"originals";harness=w/"harness";name="ConstructorArgumentRegionsHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/(name+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(6,13):raise RuntimeError("Owned constructor definition inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded=all_classes-{"Lfixture/Core;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in ("fixture/First","fixture/Second","fixture/Context","fixture/Probe","fixture/Trace"):jar.writestr(owner+".class",(originals/(owner+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for nested in range(2):
     for mode in range(22):
      if nested and mode in (2,3,6,7,12,13):continue
      text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(nested),str(mode)],f"{label}-{route}-{nested}-{mode}")
      if text!="attempts=2,constructor-regions=matched\n":raise RuntimeError("Frozen constructor observer changed")
      out.append(text)
   if len(out)!=114:raise RuntimeError("Constructor case inventory changed")
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  text=(fixture/"java/fixture/Core.java").read_text();left="context.left(seed)";right="context.right(seed)";first="new First(left,context==null?null:context.left(seed))";second="new Second(right,context==null?null:context.right(seed))";inner="new Core((Object)null,(Second)null,context)"
  if any(text.count(s)!=1 for s in (left,first,second,inner)):raise RuntimeError("Constructor mutation anchor changed")
  def helper(source,member):
   if not source.endswith("}\n"):raise RuntimeError("Constructor source tail changed")
   return source[:-2]+member+"\n}\n"
  wrong=[("swapped-getters",text.replace(left,"context.__swap(seed)").replace(right,left).replace("context.__swap(seed)",right),0,0),
   ("lost-getter",text.replace(left,"null"),0,0),
   ("double-getter",text.replace(left,"context.left(context.left(seed))"),0,0),
   ("wrong-seed",text.replace(left,"context.left(null)"),0,0),
   ("late-first-allocation",helper(text.replace(first,"makeFirst(left,context,seed)"),"private static First makeFirst(long value,Context context,Object seed){Object token=context==null?null:context.left(seed);return new First(value,token);}"),0,0),
   ("late-second-allocation",helper(text.replace(second,"makeSecond(right,context,seed)"),"private static Second makeSecond(int value,Context context,Object seed){Object token=context==null?null:context.right(seed);return new Second(value,token);}"),0,0),
   ("wrapped-getter",helper(text.replace(left,"safeLeft(context,seed)"),"private static Object safeLeft(Context context,Object seed){try{return context.left(seed);}catch(Throwable error){throw new IllegalStateException(error);}}"),0,2),
   ("narrowed-wide",text.replace("new First(left,","new First((int)left,"),0,0),
   ("wrong-wide-conversion",text.replace("(int)right","(int)(right >>> 32)"),1,0),
   ("double-inner-allocation",text.replace(inner,"new Core("+inner+",(Second)null,context)"),1,0)]
  for label,replacement,nested,mode in wrong:
   source=w/label/"Core.java";source.parent.mkdir();source.open("x").write(replacement);compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(nested),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned constructor regions" not in result.stderr:raise RuntimeError("Observer missed wrong constructor program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp += ["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned constructor definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage["minimum_java_release"]!=25:raise RuntimeError("Constructor coverage, scope or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=25);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Constructor source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);versions={}
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if (wanted["major"],wanted["minor"],actual["major"],actual["minor"])!=(52,0,69,0):raise RuntimeError("Unexpected constructor compiler class version")
   versions[owner]={"original_major":52,"rebuilt_major":69,"minor":0};wanted["major"]=69
   if actual!=wanted:raise RuntimeError("Constructor metadata changed beyond reported helpers and compiler version: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Constructor Exceptions changed")
  (w/"compiler-class-version-differences.json").open("x").write(json.dumps(versions,indent=2)+"\n")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Constructor allocation, delegation, exceptions or partial effects changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen constructor input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":25,"fresh_jvm_cases":114,"attempts_per_case":2,"wrong_programs":10,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
