#!/usr/bin/env python3
"""Verify package generic carriers retain producer tokens and independent contexts."""
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
 p.add_argument("--dependency-provider",action="store_true");p.add_argument("--package-provider",action="store_true");p.add_argument("--protected-provider",action="store_true");args=p.parse_args()
 if args.package_provider and args.protected_provider:raise RuntimeError("Choose one owned provider access profile")
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-context-carrier-token-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"context_carrier_token";originals=w/"originals";harness=w/"harness";name="fixture.ContextCarrierTokenHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk)
  source_root=fixture/"java"
  if args.package_provider or args.protected_provider:
   import shutil
   source_root=w/"package-source";shutil.copytree(fixture/"java",source_root)
   source=source_root/"fixture/Provider.java";text=source.read_text()
   if text.count("public final class Provider")!=1 or text.count("public static <T> Box<T> read")!=1:raise RuntimeError("Package context array declaration changed")
   if args.protected_provider:text=text.replace("public static <T> Box<T> read","protected static <T> Box<T> read")
   else:text=text.replace("public final class Provider","final class Provider").replace("public static <T> Box<T> read","static <T> Box<T> read")
   source.write_text(text)
  if args.package_provider and args.dependency_provider:raise RuntimeError("Package source profile needs the owned provider")
  compiler.compile_local(sorted(source_root.rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/(name.rsplit(".",1)[-1]+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(5,11):raise RuntimeError("Owned context-carrier-token definition inventory changed")
  facts=compiler_facts(originals)
  expected_signature="<T:Ljava/lang/Object;>(Lfixture/Context;Ljava/lang/String;Ljava/lang/Class<+TT;>;)Lfixture/Box<TT;>;"
  copies=[row for key,row in facts["Lfixture/Provider;"]["methods"].items() if "->read(" in key]
  if len(copies)!=1 or copies[0].get("signature")!=expected_signature:raise RuntimeError("Owned array type-token complete signature changed")
  exceptions=exception_inventory(originals);excluded=all_classes-{"Lfixture/Core;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in sorted(all_classes-{"Lfixture/Core;"}):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for mode in range(25):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,context-carrier-token=matched\n":raise RuntimeError("Frozen context-carrier-token observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted(source_root.rglob("*.java"))}
  call="Box result = Provider.read(context, key, token);"
  wrong=[
   ("lost-call","Core.java",call,"Box result = null;",0),
   ("repeat-call","Core.java",call,"Box result = repeated(context,key,token);",0),
   ("wrong-context","Core.java",call,"Box result = Provider.read(new Context(),key,token);",0),
   ("wrong-key","Core.java",call,"Box result = Provider.read(context,null,token);",0),
   ("wrong-token","Core.java",call,"Box result = Provider.read(context,key,Object.class);",0),
   ("wrapped-provider-failure","Core.java",call,"Box result = caught(context,key,token);",9),
   ("lost-callback","Provider.java","context.callback(key, token);",";",0),
   ("late-field-read","Provider.java","return result;","return (Box<T>)context.reply;",16),
   ("lost-entry-observer","Provider.java","Trace.hit(1, context, key, token, null);",";",0),
   ("lost-result-observer","Provider.java","Trace.hit(2, context, key, token, result);",";",0),
   ("lost-suffix-observer","Core.java","Trace.hit(4, context, key, token, result);",";",0),
   ("early-element-cast","Core.java",call,"Box result = Provider.read(context,key,token); String eager = (String)result.item;",12),
   ("cloned-result","Provider.java","return result;","return new Box<T>(result.item);",0)]
  for label,filename,anchor,replacement,mode in wrong:
   text=texts[filename]
   if text.count(anchor)!=1:raise RuntimeError("Context carrier token mutation anchor changed: "+label)
   replacement=text.replace(anchor,replacement)
   if label=="repeat-call":
    replacement=replacement.rsplit("}",1)[0]+"private static Box repeated(Context context,String key,Class token){Provider.read(context,key,token);return Provider.read(context,key,token); }\n}\n"
   if label=="wrapped-provider-failure":
    replacement=replacement.rsplit("}",1)[0]+"private static Box caught(Context context,String key,Class token){try{return Provider.read(context,key,token);}catch(Throwable failure){throw new IllegalStateException(failure);}}\n}\n"
   source=w/label/filename;source.parent.mkdir();source.open("x").write(replacement);compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned context carrier token mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong context carrier token program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned context-carrier-token definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Array-type-token coverage or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Array-type-token source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Array-type-token metadata changed beyond reported helpers and required Java release: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Array-type-token Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Context carrier token effects, casts, returned identity or failures changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen context-carrier-token input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":75,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider,"package_provider":args.package_provider,"protected_provider":args.protected_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
