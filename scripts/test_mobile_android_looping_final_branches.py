#!/usr/bin/env python3
"""Verify ordered final writes across unprotected constructor branch loops."""
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
 p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-looping-final-branches-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"looping_final_branches";originals=w/"originals";harness=w/"harness";name="fixture.LoopingFinalBranchesHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  texts={f.name:f.read_text() for f in sorted((fixture/"java").rglob("*.java"))}
  sources=w/"original-sources";sources.mkdir()
  for filename,text in texts.items():(sources/filename).open("x").write(text)
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted(sources.glob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/(name.rsplit(".",1)[-1]+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(3,10):raise RuntimeError("Owned looping final branches inventory changed")
  facts=compiler_facts(originals);constructor=facts["Lfixture/Core;"]["methods"]["Lfixture/Core;-><init>([Lfixture/Token;)V"]
  if constructor["access"]!=129:raise RuntimeError("Owned constructor varargs access changed")
  exceptions=exception_inventory(originals);parents={"Lfixture/Trace;","Lfixture/Token;"};excluded=parents if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in sorted(parents):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for mode in range(25):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,looping-final-branches=matched\n":raise RuntimeError("Frozen looping final branches observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  array_stage="this.values=copy;\n            Trace.hit(1,this,input,copy,-1,null);\n            this.text=Trace.text(this,input,copy,builder);"
  empty_stage="this.values=Trace.empty;\n            Trace.hit(1,this,input,this.values,-1,null);\n            this.text=Trace.emptyText;"
  wrong=[
   ("lost-clone","Core.java","(Token)input[i].clone()","input[i]",16),
   ("copied-input","Core.java","Token[] copy=new Token[input.length];","input=input.clone();Token[] copy=new Token[input.length];",17),
   ("wrong-empty-array","Core.java","this.values=Trace.empty;","this.values=new Token[0];",0),
   ("early-empty-text","Core.java",empty_stage,"String saved=Trace.emptyText;\n            this.values=Trace.empty;\n            Trace.hit(1,this,input,this.values,-1,null);\n            this.text=saved;",21),
   ("early-text-provider","Core.java",array_stage,"String saved=Trace.text(this,input,copy,builder);\n            this.values=copy;\n            Trace.hit(1,this,input,copy,-1,null);\n            this.text=saved;",16),
   ("lost-text-provider","Core.java","this.text=Trace.text(this,input,copy,builder);","this.text=null;",16),
   ("lost-array-commit","Core.java","this.values=copy;","this.values=Trace.empty;",16),
   ("lost-step-observer","Core.java","Trace.hit(5,this,input,copy,i,made);",";",16),
   ("wrong-loop-step","Core.java","++i","i+=2",16),
   ("late-array-source","Core.java","return values;","return Trace.empty;",22),
   ("late-text-source","Core.java","return text;","return Trace.textReply;",20),
   ("repeated-clone","Token.java","return Trace.clone(this);","Trace.clone(this);return Trace.clone(this);",16),
   ("wrapped-clone-failure","Token.java","return Trace.clone(this);","try{return Trace.clone(this);}catch(Throwable value){throw new IllegalStateException(value);}",8),
   ("wrong-clone-result","Trace.java","return new Token(copiedId);","return item;",16),
   ("early-text-read","Trace.java","hit(4,subject,input,copy,-1,builder);\n        return textReply;","String saved=textReply;\n        hit(4,subject,input,copy,-1,builder);\n        return saved;",19)]
  for label,filename,anchor,replacement,mode in wrong:
   text=texts[filename]
   if text.count(anchor)!=1:raise RuntimeError("Looping final mutation anchor changed: "+label)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned looping final branches mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong looping final program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned looping final definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Looping final coverage or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Looping final source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Looping final compiler metadata changed: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Looping final Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Looping final order, mutations, returned identity or failures changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen looping final input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":75,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
