#!/usr/bin/env python3
"""Verify ordered final stores and protected cleanup in a real constructor."""
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
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-ordered-final-stages-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"ordered_final_stages";originals=w/"originals";harness=w/"harness";name="OrderedFinalStagesHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/(name+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(3,9):raise RuntimeError("Owned ordered-stage definition inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded=all_classes-{"Lfixture/Core;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in ("fixture/Context","fixture/Trace"):jar.writestr(owner+".class",(originals/(owner+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for branch in range(2):
     for mode in range(20):
      text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(branch),str(mode)],f"{label}-{route}-{branch}-{mode}")
      if text!="attempts=2,ordered-final-stages=matched\n":raise RuntimeError("Frozen ordered-stage observer changed")
      out.append(text)
   if len(out)!=120:raise RuntimeError("Ordered-stage case inventory changed")
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  text=(fixture/"java/fixture/Core.java").read_text()
  cleanup="context.cleanup(this, token);";left="context.left(this, seed, token)";right="context.right(this, seed, token)"
  for anchor in (cleanup,left,right,"choice = true;","payload = ","context.finish(this, seed)"):
   if text.count(anchor)!=(2 if anchor=="payload = " else 1):raise RuntimeError("Ordered-stage mutation anchor changed")
  def helper(source,member):
   if not source.endswith("}\n"):raise RuntimeError("Ordered-stage source tail changed")
   return source[:-2]+member+"\n}\n"
  delayed=text.replace("        try {","        Object pending;\n        try {").replace("payload = ","pending = ").replace("        tail = ","        payload = pending;\n        tail = ")
  delayed_choice=text.replace("choice = true;\n                payload = "+left+";","Object pending = "+left+";\n                choice = true;\n                payload = pending;")
  wrong=[("swapped-producers",text.replace(left,"context.__swap(this, seed, token)").replace(right,left).replace("context.__swap(this, seed, token)",right),1,0),
   ("lost-cleanup",text.replace(cleanup,";"),1,0),
   ("double-cleanup",text.replace(cleanup,cleanup+cleanup),1,0),
   ("wrong-cleanup-token",text.replace(cleanup,"context.cleanup(this, null);"),1,0),
   ("wrong-choice",text.replace("choice = true;","choice = false;"),1,0),
   ("delayed-choice",delayed_choice,1,0),
   ("delayed-payload",delayed,1,0),
   ("narrowed-finish",text.replace("context.finish(this, seed)","context.finish(this, (int)seed)"),1,0),
   ("wrapped-producer",helper(text.replace(left,"safeLeft(context, this, seed, token)"),"private static Object safeLeft(Context c,Core self,long seed,Object token){try{return c.left(self,seed,token);}catch(Throwable error){throw new IllegalStateException(error);}}"),1,5),
   ("wrapped-cleanup",helper(text.replace(cleanup,"safeCleanup(context, this, token);"),"private static void safeCleanup(Context c,Core self,Object token){try{c.cleanup(self,token);}catch(Throwable error){throw new IllegalStateException(error);}}"),1,7),
   ("wrong-marker-token",text.replace("context.marker(this, token)","context.marker(this, null)"),1,0),
   ("narrowed-entry",text.replace("context.enter(this, seed, token)","context.enter(this, (int)seed, token)"),1,0)]
  for label,replacement,branch,mode in wrong:
   if replacement==text:raise RuntimeError("Ordered-stage counterexample did not change the program")
   source=w/label/"Core.java";source.parent.mkdir();source.open("x").write(replacement);compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(branch),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned ordered final stages" not in result.stderr:raise RuntimeError("Observer missed wrong ordered-stage program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp += ["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned ordered-stage definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Ordered-stage coverage, scope or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Ordered-stage source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);versions={}
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if (wanted["major"],wanted["minor"],actual["major"],actual["minor"])!=(52,0,52,0):raise RuntimeError("Unexpected ordered-stage compiler class version")
   versions[owner]={"original_major":52,"rebuilt_major":52,"minor":0};wanted["major"]=52
   if actual!=wanted:raise RuntimeError("Ordered-stage metadata changed beyond reported helpers: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Ordered-stage Exceptions changed")
  (w/"compiler-class-versions.json").open("x").write(json.dumps(versions,indent=2)+"\n")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Final field timing, cleanup, exceptions or partial effects changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen ordered-stage input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":120,"attempts_per_case":2,"wrong_programs":12,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
