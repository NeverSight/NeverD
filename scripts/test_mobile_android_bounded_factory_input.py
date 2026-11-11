#!/usr/bin/env python3
"""Verify a nominally bounded producer with an independent fixed factory result."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,subprocess,tempfile,zipfile,shutil
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
 p.add_argument("--dependency-provider",action="store_true");p.add_argument("--unused-bound",action="store_true",help="Keep the known method bound absent from all runtime slots");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-bounded-factory-input-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"bounded_factory_input";source_root=fixture/"java";originals=w/"originals";harness=w/"harness";name="BoundedFactoryInputHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  if args.unused_bound:
   authored=w/"unused-bound-source";shutil.copytree(source_root,authored);source_root=authored
   path=source_root/"fixture/Core.java";text=path.read_text()
   if text.count("? extends T>")!=2 or text.count("T value =")!=1:raise RuntimeError("Owned bounded input source anchors changed")
   path.write_text(text.replace("? extends T>","? extends Base>").replace("T value =","Base value ="))
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted(source_root.rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/(name+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(8,15):raise RuntimeError("Owned bounded-factory-input definition inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded=all_classes-{"Lfixture/Core;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in sorted(all_classes-{"Lfixture/Core;"}):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for mode in range(24):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,bounded-factory-input=matched\n":raise RuntimeError("Frozen bounded-factory-input observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  text=(source_root/"fixture/Core.java").read_text();value_type="Base" if args.unused_bound else "T";callback="producer.produce(Trace.argument(config))";finish="Trace.finish(value, config, bits)"
  def helper(source,member):
   if not source.endswith("}\n"):raise RuntimeError("Bounded factory source anchor changed")
   return source[:-2]+member+"\n}\n"
  wrong=[
   ("lost-producer",text.replace(callback,"null"),0),
   ("duplicate-producer",text.replace(value_type+" value = "+callback+";","producer.produce(Trace.argument(config)); "+value_type+" value = "+callback+";"),0),
   ("lost-config",text.replace("make(producer, config, bits)","make(producer, null, bits)"),0),
   ("lost-wide-word",text.replace("make(producer, config, bits)","make(producer, config, (int)bits)"),20),
   ("lost-result",text.replace("return result;","return null;"),0),
   ("early-null-shortcut",text.replace(value_type+" value = "+callback+";","if(producer==null) throw new NullPointerException(); "+value_type+" value = "+callback+";"),3),
   ("wrong-callback-input",text.replace(callback,"producer.produce(null)"),0),
   ("duplicate-finish",text.replace("Factory<Leaf> result = "+finish+";",finish+"; Factory<Leaf> result = "+finish+";"),0),
   ("late-result-read",text.replace("Trace.hit(4, producer, config, bits, result);\n        return result;","Trace.hit(4, producer, config, bits, result);\n        return "+finish+";"),23),
   ("lost-prefix",text.replace("Trace.hit(1, producer, config, bits, null);",";"),0),
   ("lost-item-observation",text.replace("Trace.hit(3, producer, config, bits, value);",";"),0),
   ("lost-suffix",text.replace("Trace.hit(5, producer, config, bits, result);",";"),0),
   ("wrapped-producer-failure",helper(text.replace(callback,"safe(producer,config)"),"private static <T extends Base> T safe(Producer<? super Context,? extends T> producer,Config config){try{return producer.produce(Trace.argument(config));}catch(Throwable failure){throw new IllegalStateException(failure);}}"),7),
   ("wrapped-finish-failure",helper(text.replace(finish,"safeFinish(value,config,bits)"),"private static Factory<Leaf> safeFinish(Base value,Config config,long bits){try{return Trace.finish(value,config,bits);}catch(Throwable failure){throw new IllegalStateException(failure);}}"),14)]
  for label,replacement,mode in wrong:
   if replacement==text:raise RuntimeError("Bounded factory counterexample did not change the program")
   source=w/label/"Core.java";source.parent.mkdir();source.open("x").write(replacement);compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned bounded factory input mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong bounded factory program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned bounded-factory-input definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Bounded-factory-input coverage or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Bounded-factory-input source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Bounded-factory-input metadata changed beyond reported helpers: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Bounded-factory-input Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Bounded producer input, fixed result, callback or failure identity changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen bounded-factory-input input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":72,"unused_bound":args.unused_bound,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
