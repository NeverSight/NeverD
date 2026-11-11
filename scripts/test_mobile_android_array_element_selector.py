#!/usr/bin/env python3
"""Verify array-bound element results with complete unary consumer contracts."""
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
 modes=p.add_mutually_exclusive_group();modes.add_argument("--dependency-provider",action="store_true");modes.add_argument("--private-provider",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-array-element-selector-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"array_element_selector";originals=w/"originals";harness=w/"harness";name="fixture.ArrayElementSelectorHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  texts={f.name:f.read_text() for f in sorted((fixture/"java").rglob("*.java"))}
  if args.private_provider:
   texts["Core.java"]=texts["Core.java"].replace("public class Core {","class Core {").replace("public static interface Selector<E>","private static interface Selector<E>").replace("public static <T> T choose(","private static <T> T choose(")
   texts["Client.java"]=texts["Client.java"].replace("Core.choose((Object[])values,mode,(Core.Selector)rule)","Core.entry(values,mode,rule)")
  sources=w/"original-sources";sources.mkdir()
  for filename,text in texts.items():(sources/filename).open("x").write(text)
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted(sources.glob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/(name.rsplit(".",1)[-1]+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(5,15):raise RuntimeError("Owned array element selector inventory changed")
  facts=compiler_facts(originals);signatures={row["signature"] for key,row in facts["Lfixture/Core;"]["methods"].items() if "->choose(" in key}
  expected={"<T:Ljava/lang/Object;>([TT;I"+tail+"Lfixture/Core$Selector<TT;>;)TT;" for tail in ("","Z")}
  if signatures!=expected:raise RuntimeError("Owned array selector complete signatures changed")
  exceptions=exception_inventory(originals);parents={"Lfixture/Core;","Lfixture/Core$Selector;","Lfixture/Core$Checker;","Lfixture/Trace;"};excluded=parents if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in sorted(parents):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for mode in range(25):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,array-element-selector=matched\n":raise RuntimeError("Frozen array element selector observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  call="Core.entry(values,mode,rule)" if args.private_provider else "Core.choose((Object[])values,mode,(Core.Selector)rule)"
  sequence="int rank=rule.rank(first);\n        boolean flag=rule.flag(first);\n        T result=values[rank+(flag==wanted?0:Trace.delta)];"
  wrong=[
   ("lost-entry","Core.java","Trace.hit(1,values,mode,rule,null);",";",0,0),
   ("copied-array","Core.java","return choose(values,mode,Trace.want,rule);","return choose(values.clone(),mode,Trace.want,rule);",0,0),
   ("wrong-first-index","Core.java","T first=values[Trace.index];","T first=values[0];",1,0),
   ("lost-rank-call","Core.java","int rank=rule.rank(first);","int rank=Trace.rank;",0,0),
   ("lost-flag-call","Core.java","boolean flag=rule.flag(first);","boolean flag=Trace.flag;",0,0),
   ("early-result","Core.java",sequence,"T result=values[Trace.rank+(Trace.flag==wanted?0:Trace.delta)];\n        int rank=rule.rank(first);\n        boolean flag=rule.flag(first);",19,0),
   ("wrong-rank","Core.java","int rank=rule.rank(first);","int rank=rule.rank(first)+1;",0,0),
   ("late-first-read","Core.java","boolean flag=rule.flag(first);","boolean flag=rule.flag(values[Trace.index]);",18,0),
   ("lost-boolean-snapshot","Core.java","flag==wanted","flag==Trace.want",24,0),
   ("late-result-read","Core.java","return result;","return values[rank+(flag==wanted?0:Trace.delta)];",21,0),
   ("wrong-result","Core.java","return result;","return null;",0,0),
   ("lost-ignored-call","Client.java","        "+call+";","        ;",0,2),
   ("early-result-cast","Client.java","Object result="+call+";","Object result=early(values,mode,rule);",23,0),
   ("wrapped-failure","Client.java","Object result="+call+";","Object result=caught(values,mode,rule);",13,0),
   ("repeated-call","Client.java","Object result="+call+";","Object result=repeated(values,mode,rule);",0,0)]
  for label,filename,anchor,replacement,mode,route in wrong:
   text=texts[filename]
   if text.count(anchor)!=1:raise RuntimeError("Array selector mutation anchor changed: "+label)
   text=text.replace(anchor,replacement)
   helpers={"early-result-cast":"private static Object early(Object array,int mask,Object rule){String check=(String)Trace.reply;return Core.entry(array,mask,rule);}","wrapped-failure":"private static Object caught(Object array,int mask,Object rule){try{return Core.entry(array,mask,rule);}catch(Throwable value){throw new IllegalStateException(value);}}","repeated-call":"private static Object repeated(Object array,int mask,Object rule){Core.entry(array,mask,rule);return Core.entry(array,mask,rule);}"}
   if label in helpers:text=text.rsplit("}",1)[0]+helpers[label]+"\n}\n"
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text);compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,str(route),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned array element selector mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong array element selector program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned array selector definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Array selector coverage or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Array selector source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Array selector compiler metadata changed: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Array selector Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Array selector order, mutations, returned identity or failures changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen array selector input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":75,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider,"private_provider":args.private_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
