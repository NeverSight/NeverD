#!/usr/bin/env python3
"""Verify private and protected scoped interface tokens retain opaque virtual results."""
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
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scoped-interface-token-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"scoped_interface_token";originals=w/"originals";harness=w/"harness";name="ScopedInterfaceTokenHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);cli_hash=sha(args.neverd);source_root=fixture/"java"
  compiler.compile_local(sorted(source_root.rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness/ScopedInterfaceTokenHarness.java"],"javac-observer")
  frozen={"originals":hashes(originals),"harness":hashes(harness)};(w/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(7,14):raise RuntimeError("Owned scoped interface token inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals)
  ref="Lfixture/Proxy;->adapt(Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/annotation/Annotation;"
  if facts["Lfixture/Proxy;"]["methods"][ref]["signature"]!="<T::Ljava/lang/annotation/Annotation;>(Ljava/lang/String;Ljava/lang/Class<TT;>;)TT;":raise RuntimeError("Owned scoped interface token signature changed")
  excluded={"Lfixture/Bound;","Lfixture/Context;","Lfixture/Factory;","Lfixture/Entry;"} if args.sdk_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] not in excluded};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for descriptor in sorted(excluded):
    path=descriptor[1:-1]+".class";jar.write(originals/path,path)
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(2):
    for mode in range(15):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,scoped-interface-token=matched\n":raise RuntimeError("Owned scoped interface token observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted(source_root.rglob("*.java"))}
  call="Bound result=(Bound)adapt(n,t);"
  wrong=[
   ("lost-call","Proxy.java",call,"Bound result=null;",0,0),
   ("repeated-call","Proxy.java",call,"adapt(n,t);"+call,0,0),
   ("wrong-name","Proxy.java","create(name,token,context)","create(null,token,context)",0,0),
   ("wrong-token","Proxy.java","create(name,token,context)","create(name,null,context)",0,0),
   ("late-context","Proxy.java","create(name,token,context)","create(name,token,Trace.current)",0,0),
   ("lost-prefix","Proxy.java","Trace.hit(1,this,name,token,context,null);",";",0,0),
   ("lost-ignored-call","Proxy.java","        adapt(n,t);","        ;",1,0),
   ("late-result","Proxy.java","return (T)result;","return (T)context.reply;",0,0),
   ("early-payload","Proxy.java","Trace.hit(3,this,n,t,null,result);return (String)result.value;","String early=(String)result.value;Trace.hit(3,this,n,t,null,result);return early;",0,11),
   ("wrapped-failure","Trace.java","if(observer!=null)observer.observe(phase,receiver,name,token,context,result);","try{if(observer!=null)observer.observe(phase,receiver,name,token,context,result);}catch(Throwable failure){throw new IllegalStateException(failure);}",0,8)]
  for label,filename,anchor,replacement,route,mode in wrong:
   text=texts[filename]
   if not text.count(anchor):raise RuntimeError("Owned mutation anchor changed: "+label)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,str(route),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned scoped interface token mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong scoped interface token program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned scoped interface token definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Owned scoped interface token original coverage changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Owned scoped interface token source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Owned scoped interface token compiler metadata changed: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Owned scoped interface token Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Owned scoped interface token effects or results changed")
  try:
   from scripts.test_mobile_android_nested_return import change_signature
   from scripts.test_mobile_android_static_defaults import method_code
  except ModuleNotFoundError as error:
   if error.name!="scripts":raise
   from test_mobile_android_nested_return import change_signature
   from test_mobile_android_static_defaults import method_code
  signature="<T::Ljava/lang/annotation/Annotation;>(Ljava/lang/String;Ljava/lang/Class<TT;>;Lfixture/Context;)TT;"
  controls=[
   ("covariant-token",signature.replace("Class<TT;>","Class<+TT;>")),
   ("unbounded-token",signature.replace("Class<TT;>","Class<*>")),
   ("unknown-formal",signature.replace("Class<TT;>","Class<TMissing;>")),
   ("class-owned-token",signature.replace("<T::","<U::").replace(")TT;",")TU;")),
   ("extra-formal",signature.replace("<T::","<Unused:Ljava/lang/Object;T::")),
   ("wrong-bound-kind",signature.replace("<T::","<T:")),
   ("extra-interface-bound",signature.replace("Annotation;>","Annotation;:Ljava/io/Serializable;>")),
   ("fixed-result",signature.replace(")TT;",")Ljava/lang/annotation/Annotation;")),
  ]
  for label,changed in controls:
   negative=w/(label+"-classes");negative.mkdir()
   for source in sorted(originals.rglob("*.class")):
    target=negative/source.relative_to(originals);target.parent.mkdir(parents=True,exist_ok=True);target.open("xb").write(source.read_bytes())
   target=negative/"fixture/Factory.class";before=target.read_bytes();receipt=change_signature(target,signature.encode(),changed.encode())
   (w/(label+"-metadata.json")).open("x").write(json.dumps(receipt,indent=2)+"\n")
   if method_code(before)!=method_code(target.read_bytes()):raise RuntimeError("Signature control changed original Code")
   declaration=cp
   if excluded:
    dependency=w/(label+"-provider.jar")
    with zipfile.ZipFile(dependency,"x") as jar:
     for descriptor in sorted(excluded):
      path=descriptor[1:-1]+".class";jar.write(negative/path,path)
    declaration=["--classpath",sdk,"--classpath",dependency]
   encoded=w/(label+"-dex");encoded.mkdir();v.run([*v.d8,"--release","--min-api","24",*declaration,"--output",encoded,*[negative/f.relative_to(originals) for f in paths]],"d8-"+label)
   rejected=w/(label+"-rejected");result=subprocess.run([str(args.neverd.resolve()),"mobile",str(encoded/"classes.dex"),*map(str,declaration),"-o",str(rejected)],text=True,capture_output=True,timeout=1200,env=v.environment)
   (w/(label+"-recovery.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or rejected.exists() or "generic" not in result.stderr:raise RuntimeError("Scoped interface token admitted an unproved declaration: "+label)
  if hashes(originals)!=frozen["originals"] or hashes(harness)!=frozen["harness"] or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash or sha(args.neverd)!=cli_hash:raise RuntimeError("Frozen scoped interface token input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":30,"attempts_per_case":2,"wrong_programs":len(wrong),"same_code_counterexamples":[row[0] for row in controls],"sdk_provider":args.sdk_provider,"metadata_roles_exceptions_and_runtime_definitions":"unchanged"};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
