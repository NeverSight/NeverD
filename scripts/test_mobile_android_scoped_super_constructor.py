#!/usr/bin/env python3
"""Verify independent constructor formals retain exact superclass inputs."""
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
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scoped-super-constructor-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"scoped_super_constructor";originals=w/"originals";harness=w/"harness";name="fixture.ScopedSuperConstructorHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);source_root=fixture/"java"
  compiler.compile_local(sorted(source_root.rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness/ScopedSuperConstructorHarness.java"],"javac-observer")
  frozen={"originals":hashes(originals),"harness":hashes(harness)};(w/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(4,6):raise RuntimeError("Owned scoped super constructor inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals)
  expected={"Lfixture/Base;":("K","<S:Lfixture/Base<TS;>;>Ljava/lang/Object;"),"Lfixture/Child;":("V","Lfixture/Base<Lfixture/Child;>;")}
  for owner,(formal,header) in expected.items():
   signature="<"+formal+":Ljava/lang/Object;>(T"+formal+";Lfixture/Carrier<T"+formal+";>;)V"
   if facts[owner]["signature"]!=header or facts[owner]["methods"][owner+"-><init>(Ljava/lang/Object;Lfixture/Carrier;)V"]["signature"]!=signature:raise RuntimeError("Owned independent constructor scope changed")
  excluded={"Lfixture/Base;","Lfixture/Carrier;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in ("Lfixture/Base;","Lfixture/Carrier;"):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for mode in range(10):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,scoped-super-constructor=matched\n":raise RuntimeError("Owned scoped super constructor observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted(source_root.rglob("*.java"))}
  call="super(seed,property);"
  wrong=[
   ("wrong-seed","Child.java",call,"super(null,property);",0,0),
   ("wrong-property","Child.java",call,"super(seed,(Carrier<V>)null);",0,0),
   ("lost-call","Base.java","Object result=property.apply(seed);","Object result=null;",0,0),
   ("repeated-call","Base.java","Object result=property.apply(seed);","property.apply(seed);Object result=property.apply(seed);",0,0),
   ("wrong-callback-input","Base.java","property.apply(seed)","property.apply(null)",0,0),
   ("returned-seed","Base.java","snapshot=result;","snapshot=seed;",0,0),
   ("lost-seed-field","Base.java","this.seed=seed;","this.seed=null;",0,0),
   ("lost-property-field","Base.java","this.property=property;","this.property=null;",0,0),
   ("lost-entry-observer","Base.java","Trace.hit(1,this,seed,property,null);",";",0,0),
   ("lost-callback-observer","Base.java","Trace.hit(2,this,seed,property,null);",";",0,0),
   ("lost-result-observer","Base.java","Trace.hit(3,this,seed,property,result);",";",0,0),
   ("wrong-result-observer","Base.java","Trace.hit(3,this,seed,property,result);","Trace.hit(3,this,seed,property,null);",0,0),
   ("lost-suffix-field","Child.java","stage=1;","stage=0;",0,0),
   ("lost-suffix-effect","Child.java","Trace.hit(4,this,seed,property,snapshot);",";",0,0),
   ("repeated-suffix-effect","Child.java","Trace.hit(4,this,seed,property,snapshot);","Trace.hit(4,this,seed,property,snapshot);Trace.hit(4,this,seed,property,snapshot);",0,0),
   ("wrapped-failure","Trace.java","if(current!=null)current.observe(phase,parent,seed,property,result);","try{if(current!=null)current.observe(phase,parent,seed,property,result);}catch(Throwable failure){throw new IllegalStateException(failure);}",0,2)]
  for label,filename,anchor,replacement,route,mode in wrong:
   text=texts[filename]
   if not text.count(anchor):raise RuntimeError("Owned mutation anchor changed: "+label)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,str(route),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned scoped super constructor mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong scoped super constructor program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned scoped super constructor definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Owned scoped super constructor original coverage changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Owned scoped super constructor source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Owned scoped super constructor compiler metadata changed: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Owned scoped super constructor Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Owned scoped super constructor effects or results changed")
  if hashes(originals)!=frozen["originals"] or hashes(harness)!=frozen["harness"] or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen scoped super constructor input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":30,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
