#!/usr/bin/env python3
"""Verify paired private callback factories keep closed context and opaque results."""
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
 p.add_argument("--sdk-provider",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-paired-callback-factory-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"paired_callback_factory";originals=w/"originals";harness=w/"harness";name="fixture.PairedCallbackFactoryHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);source_root=fixture/"java"
  compiler.compile_local(sorted(source_root.rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness/PairedCallbackFactoryHarness.java"],"javac-observer")
  frozen={"originals":hashes(originals),"harness":hashes(harness)};(w/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(7,12):raise RuntimeError("Owned paired callback factory inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals)
  ref="Lfixture/Host;->make(Lfixture/Pair;Lfixture/Function;Lfixture/Callback;)Lfixture/Box;"
  if facts["Lfixture/Host;"]["methods"][ref]["signature"]!="<I:Ljava/lang/Object;O:Ljava/lang/Object;>(Lfixture/Pair<TI;TO;>;Lfixture/Function<Ljava/lang/Void;Lfixture/Registry;>;Lfixture/Callback<TO;>;)Lfixture/Box<TI;>;":raise RuntimeError("Owned private paired callback signature changed")
  excluded={"Lfixture/Pair;","Lfixture/Function;","Lfixture/Callback;","Lfixture/Box;","Lfixture/Registry;"} if args.sdk_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] not in excluded};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for descriptor in sorted(excluded):
    path=descriptor[1:-1]+".class";jar.write(originals/path,path)
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(2):
    for mode in range(16):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,paired-callback-factory=matched\n":raise RuntimeError("Owned paired callback factory observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted(source_root.rglob("*.java"))}
  call="Box result=make(input,context,callback);"
  wrong=[
   ("lost-call","Host.java",call,"Box result=null;",0,0),
   ("repeated-call","Host.java",call,"make(input,context,callback);"+call,0,0),
   ("wrong-input","Host.java",call,call.replace("make(input,","make(null,"),0,0),
   ("wrong-context","Host.java",call,call.replace("input,context,","input,null,"),0,0),
   ("wrong-callback","Host.java",call,call.replace("context,callback","context,null"),0,0),
   ("wrong-receiver","Host.java",call,call.replace("result=make(","result=new Host().make("),0,0),
   ("lost-context-call","Host.java","Registry registry=context.apply(null);","Registry registry=null;",0,0),
   ("repeated-context-call","Host.java","Registry registry=context.apply(null);","context.apply(null);Registry registry=context.apply(null);",0,0),
   ("lost-callback","Host.java","callback.accept(value);",";",0,0),
   ("repeated-callback","Host.java","callback.accept(value);","callback.accept(value);callback.accept(value);",0,0),
   ("early-output-read","Host.java","Registry registry=context.apply(null);\n        O value=input.output;","O value=input.output;Registry registry=context.apply(null);",0,0),
   ("early-box-read","Host.java","callback.accept(value);\n        Box<I> result=input.box;","Box<I> result=input.box;callback.accept(value);",0,0),
   ("late-box-read","Host.java","return result;","return input.box;",0,0),
   ("lost-ignored-call","Host.java","        make(input,context,callback);","        ;",1,0),
   ("early-caller-cast","Host.java",call,call+"String eager=(String)result.value;",0,13),
   ("wrapped-failure","Trace.java","if(current!=null)current.observe(phase,owner,input,context,callback,result);","try{if(current!=null)current.observe(phase,owner,input,context,callback,result);}catch(Throwable failure){throw new IllegalStateException(failure);}",0,10)]
  for label,filename,anchor,replacement,route,mode in wrong:
   text=texts[filename]
   if not text.count(anchor):raise RuntimeError("Owned mutation anchor changed: "+label)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,str(route),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned paired callback factory mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong paired callback factory program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned paired callback factory definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Owned paired callback factory original coverage changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Owned paired callback factory source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Owned paired callback factory compiler metadata changed: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Owned paired callback factory Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Owned paired callback factory effects or results changed")
  if hashes(originals)!=frozen["originals"] or hashes(harness)!=frozen["harness"] or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen paired callback factory input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":32,"attempts_per_case":2,"wrong_programs":len(wrong),"sdk_provider":args.sdk_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
