#!/usr/bin/env python3
"""Verify canonical empty package-info classes retain compiler and package identities."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,shutil,struct,subprocess,tempfile,zipfile
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

def compile_sources(v,sources,out,classpath=None,options=()):
 empty=v.work/(out.name+"-empty");empty.mkdir();out.mkdir(parents=True)
 v.run([v.javac,"--release","8","-encoding","UTF-8","-g","-proc:none","-implicit:none","-sourcepath",empty,"-processorpath",empty,"-classpath",classpath if classpath is not None else empty,*options,"-d",out,*sources],"javac-"+out.name)

def class_access_offset(data):
 if data[:4]!=b"\xca\xfe\xba\xbe":raise RuntimeError("Invalid owned class fixture")
 count=struct.unpack_from(">H",data,8)[0];offset=10;index=1
 sizes={3:4,4:4,5:8,6:8,7:2,8:2,9:4,10:4,11:4,12:4,15:3,16:2,17:4,18:4,19:2,20:2}
 while index<count:
  tag=data[offset];offset+=1
  if tag==1:
   size=struct.unpack_from(">H",data,offset)[0];offset+=2+size
  elif tag in sizes:offset+=sizes[tag];index+=int(tag in (5,6))
  else:raise RuntimeError("Unknown owned compiler fixture constant")
  index+=1
 if offset+8>len(data):raise RuntimeError("Truncated owned compiler fixture")
 return offset

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ("neverd","android-jar","java-home","compiler-java-home"):p.add_argument("--"+name,type=Path,required=True)
 for name in ("d8","work-dir"):p.add_argument("--"+name,type=Path)
 p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-empty-package-metadata-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"empty_package_metadata";originals=w/"originals";harness=w/"harness";name="fixture.EmptyPackageMetadataHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();options=["-Xpkginfo:always"]
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compile_sources(compiler,sorted((fixture/"java").rglob("*.java")),originals,options=options)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness"/(name.rsplit(".",1)[-1]+".java")],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(5,7):raise RuntimeError("Owned empty package metadata definition inventory changed")
  facts=compiler_facts(originals);metadata={"Lfixture/alpha/package-info;","Lfixture/beta/package-info;"}
  for owner in metadata:
   row=facts[owner]
   if row["access"]!=0x1600 or row["signature"] is not None or row["fields"] or row["methods"]:raise RuntimeError("Original compiler package metadata changed")
  exceptions=exception_inventory(originals);parents={"Lfixture/alpha/Anchor;","Lfixture/beta/Anchor;"};excluded=parents if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in sorted(parents):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider);values=[0,1,-1,2147483647,-2147483648,305441741,1073741824,-987654321]
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for value in values:
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(value)],f"{label}-{route}-{value}")
     if text!="attempts=2,empty-package-metadata=matched\n":raise RuntimeError("Frozen empty package metadata observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.relative_to(fixture/"java").as_posix():f.read_text() for f in sorted((fixture/"java").rglob("*.java"))}
  expression="fixture.beta.Anchor.apply(fixture.alpha.Anchor.apply(value))"
  wrong=[("omitted-metadata",None,None,None,0),("renamed-metadata",None,None,None,0),("lost-synthetic-flag",None,None,None,0),("invented-annotation","fixture/alpha/package-info.java","package fixture.alpha;","@java.lang.Deprecated package fixture.alpha;",0),("wrong-arithmetic","fixture/alpha/Anchor.java","return value+3;","return value+4;",0),("lost-call","fixture/Core.java","return "+expression+";","return fixture.beta.Anchor.apply(value);",0),("repeated-call","fixture/Core.java","return "+expression+";","int result="+expression+";fixture.beta.Anchor.apply(value);return result;",0),("lost-ignored-call","fixture/Core.java","        "+expression+";","        ;",2),("wrong-order","fixture/Core.java","return "+expression+";","return fixture.alpha.Anchor.apply(fixture.beta.Anchor.apply(value));",0),("missing-compiler-option",None,None,None,0)]
  for label,filename,anchor,replacement,route in wrong:
   candidate=w/(label+"-classes")
   if label=="missing-compiler-option":compile_sources(compiler,sorted((fixture/"java").rglob("*.java")),candidate)
   else:
    shutil.copytree(originals,candidate)
    path=candidate/"fixture/alpha/package-info.class"
    if label=="omitted-metadata":path.unlink()
    elif label=="renamed-metadata":
     data=path.read_bytes();before=b"fixture/alpha/package-info";after=b"fixture/alpha/package_info"
     if len(before)!=len(after) or data.count(before)!=1:raise RuntimeError("Owned rename control changed")
     path.write_bytes(data.replace(before,after))
    elif label=="lost-synthetic-flag":
     data=bytearray(path.read_bytes());offset=class_access_offset(data)
     if struct.unpack_from(">H",data,offset)[0]!=0x1600:raise RuntimeError("Owned package flag control changed")
     struct.pack_into(">H",data,offset,0x600);path.write_bytes(data)
    else:
     text=texts[filename]
     if text.count(anchor)!=1:raise RuntimeError("Empty package mutation anchor changed: "+label)
     source=w/label/filename;source.parent.mkdir(parents=True);source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-compiled");compile_sources(compiler,[source],compiled,classpath=originals,options=options)
     for row in compiled.rglob("*.class"):
      dest=candidate/row.relative_to(compiled);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(row,dest)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(candidate,harness))),name,str(route),"0"],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned empty package metadata mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong package metadata program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned empty package metadata definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Empty package coverage or signatures changed")
  bindings=coverage.get("package_metadata_classes",[])
  if coverage.get("java_compiler_options")!=options or {r["class"] for r in bindings}!=metadata or len(bindings)!=2 or any(r["kind"]!="empty-package-info" or r["compiler_option"]!="-Xpkginfo:always" for r in bindings):raise RuntimeError("Empty package compiler identity mapping changed")
  for owner in metadata:
   source=output/"sources"/(owner[1:-1]+".java")
   if source.read_text()!="package "+owner[1:owner.rfind('/')].replace("/",".")+";\n\n":raise RuntimeError("Package metadata invented a source type or annotation")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes-metadata};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];compile_sources(v,sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,options=coverage["java_compiler_options"]);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Empty package source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Empty package compiler metadata changed: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Empty package Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Package identity, metadata or ordered ordinary calls changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen package metadata input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":24,"attempts_per_case":2,"wrong_programs":len(wrong),"package_metadata_classes":2,"java_compiler_options":options,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
