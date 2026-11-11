#!/usr/bin/env python3
"""Verify callback-only generic results preserve calls, results, and source scope."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,subprocess,tempfile,zipfile
from contextlib import nullcontext
from pathlib import Path
try:
 from scripts.mobile_real_apps_android import dex_header,parse_dexdump
 from scripts.test_mobile_android_annotation_values import compiler_inventory
 from scripts.test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
 from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
 from scripts.test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
except ModuleNotFoundError as error:
 if error.name!="scripts":raise
 from mobile_real_apps_android import dex_header,parse_dexdump
 from test_mobile_android_annotation_values import compiler_inventory
 from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
 from test_mobile_android_visibility_generic_bridge import compiler_facts
 from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ("neverd","android-jar","java-home","compiler-java-home"):p.add_argument("--"+name,type=Path,required=True)
 for name in ("d8","work-dir"):p.add_argument("--"+name,type=Path)
 p.add_argument("--package-owner",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-context-callback-result-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"context_callback_result";originals=w/"originals";harness=w/"harness";name="fixture.ContextCallbackResultHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);source_root=fixture/"java"
  if args.package_owner:
   import shutil
   source_root=w/"package-source";shutil.copytree(fixture/"java",source_root)
   public=source_root/"fixture/Provider.java";text=public.read_text()
   if text.count("public final class Provider")!=1:raise RuntimeError("Owned package declaration changed")
   public.write_text(text.replace("public final class Provider","final class Provider"))
  compiler.compile_local(sorted(source_root.rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness/ContextCallbackResultHarness.java"],"javac-observer")
  frozen={"originals":hashes(originals),"harness":hashes(harness)};(w/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(4,12):raise RuntimeError("Owned context callback result inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals)
  owner="Lfixture/Provider;";ref=owner+"->read(Ljava/lang/CharSequence;IIIZLfixture/Provider$Callback;)Ljava/lang/Object;"
  if facts[owner]["methods"][ref]["signature"]!="<T:Ljava/lang/Object;>(Ljava/lang/CharSequence;IIIZLfixture/Provider$Callback<TT;>;)TT;":raise RuntimeError("Owned callback-only result signature changed")
  callback=facts["Lfixture/Provider$Callback;"]
  if callback["signature"]!="<T:Ljava/lang/Object;>Ljava/lang/Object;" or callback["inner_class"]["access"]!=0x60a:raise RuntimeError("Owned complete private callback declaration changed")
  excluded=set();classes=all_classes;methods=set(all_methods);provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x"):pass
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(2):
    for mode in range(14):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,context-callback-result=matched\n":raise RuntimeError("Owned context callback result observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted(source_root.rglob("*.java"))}
  call="Object result=read(context,left,right,limit,flag,(Callback)callback);"
  wrong=[
   ("lost-call","Provider.java",call,"Object result=null;",0,0),
   ("repeated-call","Provider.java",call,"read(context,left,right,limit,flag,(Callback)callback);"+call,0,0),
   ("wrong-context","Provider.java",call,call.replace("read(context,","read(null,"),0,0),
   ("wrong-left","Provider.java",call,call.replace("context,left,","context,0,"),0,0),
   ("wrong-right","Provider.java",call,call.replace("left,right,","left,0,"),0,0),
   ("wrong-limit","Provider.java",call,call.replace("right,limit,","right,0,"),0,0),
   ("wrong-flag","Provider.java",call,call.replace("limit,flag,","limit,false,"),0,0),
   ("wrong-callback","Provider.java",call,call.replace("(Callback)callback","(Callback)null"),0,0),
   ("wrapped-failure","Provider.java",call,"Object result;try{result=read(context,left,right,limit,flag,(Callback)callback);}catch(Throwable failure){throw new IllegalStateException(failure);}",0,8),
   ("lost-accept","Provider.java","callback.accept(context,left,right,limit,flag);",";",0,0),
   ("repeated-result","Provider.java","T result=callback.getResult();","callback.getResult();T result=callback.getResult();",0,0),
   ("lost-result","Provider.java","T result=callback.getResult();","T result=null;",0,0),
   ("late-result-read","Provider.java","return result;","return callback.getResult();",0,0),
   ("early-caller-cast","Provider.java",call,call+"String eager=(String)result;",0,10),
   ("wrong-receiver","Provider.java",call,call.replace("result=read(","result=new Provider().read("),0,0),
   ("lost-caller-effect","Provider.java","Trace.hit(4,this,context,callback,left,right,limit,flag,result);",";",0,0)]
  for label,filename,anchor,replacement,route,mode in wrong:
   text=texts[filename]
   if not text.count(anchor):raise RuntimeError("Owned mutation anchor changed: "+label)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,str(route),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned context callback result mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong context callback result program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned context callback result definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Owned context callback result original coverage changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Owned context callback result source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Owned context callback result compiler metadata changed: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Owned context callback result Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Owned context callback result effects or results changed")
  if hashes(originals)!=frozen["originals"] or hashes(harness)!=frozen["harness"] or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen context callback result input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":28,"attempts_per_case":2,"wrong_programs":len(wrong),"package_owner":args.package_owner};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
