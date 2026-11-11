#!/usr/bin/env python3
"""Verify paired conditions before constructor delegation."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,shutil,subprocess,tempfile,zipfile
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
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-paired-constructor-conditions-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"paired_constructor_conditions";originals=w/"originals";harness=w/"harness";name="PairedConstructorConditionsHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);v.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=25)
  declarations=w/"declarations";v.compile_local([fixture/"java/fixture/Factory.java",fixture/"java/fixture/Trace.java"],declarations,classpath=originals,release=8)
  for owner in ("Factory","Trace"):shutil.copy2(declarations/("fixture/"+owner+".class"),originals/("fixture/"+owner+".class"))
  harness.mkdir();v.run([v.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/(name+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(3,6):raise RuntimeError("Owned paired-constructor-condition definition inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded=all_classes-{"Lfixture/Core;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in ("fixture/Factory","fixture/Trace"):jar.writestr(owner+".class",(originals/(owner+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for mode in range(24):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,paired-constructor-conditions=matched\n":raise RuntimeError("Frozen paired-constructor-condition observer changed")
     out.append(text)
   if len(out)!=72:raise RuntimeError("Constructor-array-store case inventory changed")
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  text=(fixture/"java/fixture/Core.java").read_text()
  def helper(source,member):
   if not source.endswith("}\n"):raise RuntimeError("Paired constructor source tail changed")
   return source[:-2]+member+"\n}\n"
  early=text.replace("        Trace.hit(2, this);\n        this.value = value;","        this.value = value;\n        Trace.hit(2, this);")
  wrong=[
   ("lost-loop",text.replace("while (index < bound)","if (index < bound)"),0),
   ("wrong-bound",text.replace("while (index < bound)","while (index <= bound)"),1),
   ("wrong-counter",text.replace("index++;","index--;"),0),
   ("unsigned-order",text.replace("while (index < bound)","while (Integer.compareUnsigned(index, bound) < 0)"),0),
   ("reversed-identity",text.replace("if (left == right)","if (left != right)"),7),
   ("lost-carried-result",text.replace("value = factory.step(value, index);","factory.step(value, index);"),0),
   ("wrong-delegated-result",text.replace("this(value);","this(left);"),0),
   ("early-final-store",early,0),
   ("early-receiver-escape",text.replace("Trace.hit(2, this);","Trace.core = this; Trace.hit(2, this);"),0),
   ("wrapped-step-failure",helper(text.replace("factory.step(value, index)","safeStep(factory, value, index)"),"private static Object safeStep(Factory f,Object v,int i){try{return f.step(v,i);}catch(Throwable failure){throw new IllegalStateException(failure);}}"),13),
   ("wrapped-constructor-failure",helper(text.replace("Trace.hit(2, this);","safeBefore(this);"),"private static void safeBefore(Core self){try{Trace.hit(2,self);}catch(Throwable failure){throw new IllegalStateException(failure);}}"),17),
   ("value-equality",text.replace("if (left == right)","if (left != null && left.equals(right))"),8)]
  for label,replacement,mode in wrong:
   if replacement==text:raise RuntimeError("Paired constructor counterexample did not change the program")
   source=w/label/"Core.java";source.parent.mkdir();source.open("x").write(replacement);compiled=w/(label+"-classes");v.compile_local([source],compiled,classpath=originals,release=25)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned paired constructor conditions" not in result.stderr:raise RuntimeError("Observer missed wrong paired constructor program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp += ["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  branches=v.run([v.dexdump,"-d",dex],"sdk-branches")
  if not re.search(r"\bif-(eq|ne|lt|ge|gt|le) v[0-9]+, v[0-9]+,",branches):raise RuntimeError("Owned original did not retain a paired condition")
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned paired-constructor-condition definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=25:raise RuntimeError("Constructor-array-store coverage, scope or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=25);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Constructor-array-store source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);versions={}
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if (wanted["major"],wanted["minor"],actual["major"],actual["minor"])!=(69 if owner=="Lfixture/Core;" else 52,0,69,0):raise RuntimeError("Unexpected paired-constructor-condition compiler class version")
   versions[owner]={"original_major":wanted["major"],"rebuilt_major":69,"minor":0};wanted["major"]=69
   if actual!=wanted:raise RuntimeError("Constructor-array-store metadata changed beyond reported helpers: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Constructor-array-store Exceptions changed")
  (w/"compiler-class-versions.json").open("x").write(json.dumps(versions,indent=2)+"\n")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Paired condition, loop, callback or final assignment changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen paired-constructor-condition input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":25,"fresh_jvm_cases":72,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
