#!/usr/bin/env python3
"""Verify javac regenerates erased visibility bridges over closed generic parent inputs."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,subprocess,tempfile,zipfile
from contextlib import nullcontext
from pathlib import Path
try:
 from scripts.mobile_real_apps_android import dex_header,parse_dexdump
 from scripts.test_mobile_android_annotation_values import compiler_inventory
 from scripts.test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
 from scripts.test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
except ModuleNotFoundError as error:
 if error.name!="scripts":raise
 from mobile_real_apps_android import dex_header,parse_dexdump
 from test_mobile_android_annotation_values import compiler_inventory
 from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
 from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory

try:
 from scripts.test_mobile_android_monitor_sparse_cleanup import MonitorClassFile
 from scripts.mobile_android_class_identity import Bytes
except ModuleNotFoundError as error:
 if error.name!="scripts":raise
 from test_mobile_android_monitor_sparse_cleanup import MonitorClassFile
 from mobile_android_class_identity import Bytes

class ParameterClassFile(MonitorClassFile):
 def deprecation(self, attributes, context, **options):
  result=super().deprecation(attributes,context,**options)
  if "->" in context and "(" in context:
   tables=[data for kind,data in attributes if kind=="MethodParameters"]
   if len(tables)>1:raise RuntimeError("Duplicate owned compiler parameter table")
   value=None
   if tables:
    reader=Bytes(tables[0]);value=[]
    for unused in range(reader.number(1)):
     name=reader.number(2);flags=reader.number(2)
     value.append({"name":self.text(name) if name else None,"access":flags})
    reader.finish()
   result["method_parameters"]=value
  return result

def compiler_facts(root):
 result={}
 for path in sorted(root.rglob("*.class")):
  row=ParameterClassFile(path.read_bytes()).facts()
  if row["name"] in result or path.relative_to(root).as_posix()!=row["name"][1:-1]+".class":raise RuntimeError("Duplicate owned visibility compiler class")
  result[row["name"]]=row
 return result

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ("neverd","android-jar","java-home","compiler-java-home"):p.add_argument("--"+name,type=Path,required=True)
 for name in ("d8","work-dir"):p.add_argument("--"+name,type=Path)
 p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-visibility-generic-bridge-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"visibility_generic_bridge";originals=w/"originals";harness=w/"harness";name="fixture.VisibilityGenericBridgeHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/(name.rsplit(".",1)[-1]+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(6,14):raise RuntimeError("Owned visibility-generic-bridge definition inventory changed")
  facts=compiler_facts(originals)
  expected_signature="(Ljava/lang/Object;Ljava/lang/Object;Ljava/util/List<[Ljava/lang/String;>;I)Ljava/lang/Object;"
  copies=[row for key,row in facts["Lfixture/Root;"]["methods"].items() if "->read(" in key]
  if len(copies)!=1 or copies[0].get("signature")!=expected_signature:raise RuntimeError("Owned visibility bridge complete parent signature changed")
  bridge=[row for key,row in facts["Lfixture/Core;"]["methods"].items() if "->read(" in key]
  if len(bridge)!=1 or bridge[0]["access"]!=4161 or bridge[0]["signature"] is not None or bridge[0]["method_parameters"]!=[{"name":None,"access":4096}]*4:raise RuntimeError("Original compiler visibility bridge or synthetic parameter metadata changed")
  exceptions=exception_inventory(originals);parents={"Lfixture/Root;","Lfixture/Mid;","Lfixture/Trace;"};excluded=parents if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in sorted(parents):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(6):
    for mode in range(25):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,visibility-generic-bridge=matched\n":raise RuntimeError("Frozen visibility-generic-bridge observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted((fixture/"java").rglob("*.java"))}
  call="Object result=self.read(first,second,values,index);"
  wrong=[
   ("lost-root-entry","Root.java","Trace.hit(1,this,first,second,values,index,null);",";",0),
   ("late-lookup","Root.java","Object result=array[Trace.offset];","Object result=values.get(index)[Trace.offset];",15),
   ("copied-array","Root.java","String[] array=values.get(index);","String[] array=values.get(index).clone();",0),
   ("wrong-index","Caller.java",call,"Object result=self.read(first,second,values,0);",1),
   ("repeat-call","Caller.java",call,"Object result=repeated(self,first,second,values,index);",0),
   ("wrapped-provider-failure","Caller.java",call,"Object result=caught(self,first,second,values,index);",11),
   ("wrong-root-result","Root.java","return result;","return values;",0),
   ("early-array-cast","Caller.java",call,"Object result=self.read(first,second,eager(values,index),index);",5),
   ("wrong-receiver","Caller.java",call,"Object result=new Core().read(first,second,values,index);",0),
   ("lost-suffix-observer","Caller.java","Trace.hit(6,self,first,second,values,index,result);",";",0),
   ("wrong-super-dispatch","Sub.java","return super.read(first,second,values,index);","return this.read(first,second,values,index);",0),
   ("lost-override-result","Sub.java","return Trace.overrideReply;","return null;",0),
   ("late-result-read","Root.java","return result;","return array[Trace.offset];",16),
   ("wrong-offset","Root.java","Object result=array[Trace.offset];","Object result=array[0];",1)]
  for label,filename,anchor,replacement,mode in wrong:
   text=texts[filename]
   if text.count(anchor)!=1:raise RuntimeError("Visibility generic bridge mutation anchor changed: "+label)
   replacement=text.replace(anchor,replacement)
   if label=="repeat-call":
    replacement=replacement.rsplit("}",1)[0]+"private static Object repeated(Core self,Object first,Object second,java.util.List values,int index){self.read(first,second,values,index);return self.read(first,second,values,index); }\n}\n"
   if label=="wrapped-provider-failure":
    replacement=replacement.rsplit("}",1)[0]+"private static Object caught(Core self,Object first,Object second,java.util.List values,int index){try{return self.read(first,second,values,index);}catch(Throwable failure){throw new IllegalStateException(failure);}}\n}\n"
   if label=="early-array-cast":
    replacement=replacement.rsplit("}",1)[0]+"private static java.util.List eager(java.util.List values,int index){String[] check=(String[])values.get(index);return values;}\n}\n"
   source=w/label/filename;source.parent.mkdir();source.open("x").write(replacement);compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,str(3 if label=="wrong-super-dispatch" else 4 if label=="lost-override-result" else 0),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned visibility generic bridge mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong visibility generic bridge program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned visibility-generic-bridge definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0)!=1 or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Visibility bridge coverage or original signatures changed")
  projected=[r for r in coverage["methods"] if r["status"]=="source-projected"]
  if len(projected)!=1 or projected[0].get("projection_kind")!="generated-public-superclass-access-bridge" or projected[0]["class"]!="Lfixture/Core;":raise RuntimeError("Original visibility bridge lost its compiler source mapping")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Visibility bridge source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Visibility bridge metadata changed beyond reported helpers and required Java release: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Visibility bridge Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Visibility generic bridge effects, casts, returned identity or failures changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen visibility-generic-bridge input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":150,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
