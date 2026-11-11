#!/usr/bin/env python3
"""Verify closed package constructors retain independent argument contracts."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,shutil,subprocess,tempfile,zipfile
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
 p.add_argument("--dependency-provider",action="store_true");p.add_argument("--nominal-sibling",action="store_true");p.add_argument("--ancestor-input",action="store_true");args=p.parse_args()
 if args.ancestor_input:args.nominal_sibling=True
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-constructor-overload-argument-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"constructor_overload_argument";originals=w/"originals";harness=w/"harness";name="fixture.ConstructorOverloadArgumentHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  if args.nominal_sibling:
   staged=w/"fixture";shutil.copytree(fixture,staged);fixture=staged
   source=fixture/"java/fixture/Core.java";text=source.read_text();anchor='    public String kind() { return "base"; }'
   if text.count(anchor)!=1:raise RuntimeError("Owned nominal sibling source anchor changed")
   text=text.replace("public class Core {", "public class Core extends Base {")
   text=text.replace("public Core(String id) {", "public Core(String id) {\n        super(id);")
   source.write_text(text.replace(anchor,'    public Core(Class<?> token) { this((String)null); }\n'+anchor))
   (fixture/"java/fixture/Base.java").write_text("package fixture; public class Base { public Base(String value) {} public Base(Class<?> token) {} }\n")
  if args.ancestor_input:
   source=fixture/"java/fixture/Core.java";text=source.read_text();anchor='    public String kind() { return "base"; }'
   source.write_text(text.replace(anchor,'    public Core(Provider provider, boolean selected) { super(provider); this.value = provider.id; }\n'+anchor))
   source=fixture/"java/fixture/Provider.java";source.write_text(source.read_text().replace("public class Provider {", "public class Provider extends Ancestor {"))
   (fixture/"java/fixture/Ancestor.java").write_text("package fixture; public class Ancestor implements java.io.Serializable {}\n")
   (fixture/"java/fixture/Base.java").write_text("""package fixture;
public class Base {
 public int kind; public Object input;
 public static int calls; public static Object last, seen;
 public Base(String value) { kind=1; input=value; calls++; last=this; seen=value; }
 public Base(Class<?> value) { kind=2; input=value; calls++; last=this; seen=value; }
 public Base(Provider value) { kind=3; input=value; calls++; last=this; seen=value; }
}
""")
  observer_sources=[FIXTURES/"harness"/(name.rsplit(".",1)[-1]+".java")]
  if args.ancestor_input:
   observer=w/"AncestorConstructorHarness.java"
   observer.write_text('package fixture;\nimport java.lang.invoke.*;\nimport java.lang.reflect.*;\npublic final class AncestorConstructorHarness {\n    static void check(boolean ok) { if (!ok) throw new AssertionError("owned ancestor constructor mismatch"); }\n    static final class Future extends Provider {\n        int reads;\n        Future() { super(new String("owned provider field")); }\n        public String id() { reads++; return "wrong virtual field read"; }\n    }\n    static Core call(int route, Provider value) throws Throwable {\n        if (route == 0) return new Core(value, true);\n        if (route == 1) return (Core)MethodHandles.lookup().findConstructor(Core.class,\n            MethodType.methodType(void.class, Provider.class, boolean.class)).invokeExact(value, true);\n        try { return Core.class.getConstructor(Provider.class, boolean.class).newInstance(value, true); }\n        catch (InvocationTargetException error) { throw error.getCause(); }\n    }\n    public static void main(String[] args) throws Throwable {\n        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);\n        for (int attempt = 0; attempt < 2; attempt++) {\n            Provider value = mode == 2 ? null : mode == 3 ? new Future() :\n                new Provider(mode == 1 ? null : new String("owned constructor value"));\n            Base.calls = 0; Base.last = null; Base.seen = new Object();\n            Core result = null; Throwable failure = null;\n            try { result = call(route, value); } catch (Throwable error) { failure = error; }\n            check(Base.calls == 1 && Base.seen == value && Base.last instanceof Core);\n            Core partial = (Core)Base.last;\n            check(partial.kind == 3 && partial.input == value && partial.stored == null);\n            if (mode == 2) check(result == null && failure != null && failure.getClass() == NullPointerException.class && partial.value == null);\n            else check(failure == null && result == partial && result.value == value.id);\n            if (mode == 3) check(((Future)value).reads == 0);\n        }\n        System.out.println("attempts=2,ancestor-constructor=matched");\n    }\n}\n')
   observer_sources.append(observer)
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,*observer_sources],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=((8,19) if args.ancestor_input else (7,16) if args.nominal_sibling else (6,13)):raise RuntimeError("Owned constructor-overload-argument definition inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded=all_classes-{"Lfixture/Core;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in sorted(all_classes-{"Lfixture/Core;"}):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for mode in range(20):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,constructor-overload-argument=matched\n":raise RuntimeError("Frozen constructor-overload-argument observer changed")
     out.append(text)
   if args.ancestor_input:
    for route in range(3):
     for mode in range(4):
      text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),"fixture.AncestorConstructorHarness",str(route),str(mode)],f"{label}-ancestor-{route}-{mode}")
      if text!="attempts=2,ancestor-constructor=matched\n":raise RuntimeError("Frozen ancestor constructor observer changed")
      out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted((fixture/"java").rglob("*.java"))}
  factory="this(provider, Factory.<Value>empty());"
  id="this(provider.id());"
  first="Trace.hit(1, this, id, null);"
  value="this.value = id;"
  before="Trace.hit(2, this, provider, values);"
  store="this.stored = values;"
  after="Trace.hit(3, this, id, null);"
  wrong=[
   ("lost-factory","Core.java",factory,"this(provider, (Box<Value>)null);",0),
   ("duplicate-factory","Core.java",factory,"this(provider, doubleBox());",0),
   ("wrong-provider","Core.java",factory,"this(null, Factory.<Value>empty());",0),
   ("lost-id-read","Core.java",id,"this((String)null);",0),
   ("duplicate-id-read","Core.java",id,"this(doubleId(provider));",0),
   ("early-final-store","Core.java",first+"\n        "+value,value+"\n        "+first,0),
   ("lost-final-observer","Core.java",after,";",0),
   ("lost-store-observer","Core.java",before,";",0),
   ("lost-store","Core.java",store,";",0),
   ("late-box-read","Core.java",store,"this.stored = (Box<Value>)Trace.next;",17),
   ("early-element-cast","Core.java",before,"Value ignored=values.value; "+before,5),
   ("wrapped-factory-failure","Factory.java","Trace.hit(0, null, null, null);","try { Trace.hit(0,null,null,null); } catch(Throwable failure) { throw new IllegalStateException(failure); }",6),
   ("wrapped-provider-failure","Provider.java","Trace.hit(5, this, id, null);","try { Trace.hit(5,this,id,null); } catch(Throwable failure) { throw new IllegalStateException(failure); }",7),
   ("wrong-final-value","Core.java",value,"this.value = null;",0)]
  if args.nominal_sibling:
   wrong.append(("wrong-nominal-sibling","Core.java",id,"this((Class<?>)null);",0))
  for label,filename,anchor,replacement,mode in wrong:
   text=texts[filename]
   if text.count(anchor)!=1:raise RuntimeError("Constructor overload argument mutation anchor changed: "+label)
   replacement=text.replace(anchor,replacement)
   if label=="duplicate-factory":
    replacement=replacement[:-2]+"private static Box<Value> doubleBox(){Factory.<Value>empty();return Factory.<Value>empty();}\n}\n"
   if label=="duplicate-id-read":
    replacement=replacement[:-2]+"private static String doubleId(Provider provider){provider.id();return provider.id();}\n}\n"
   source=w/label/filename;source.parent.mkdir();source.open("x").write(replacement);compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned constructor overload argument mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong constructor overload argument program: "+label)
  ancestor_wrong=[]
  if args.ancestor_input:
   parent="public Base(Provider value) { kind=3; input=value; calls++; last=this; seen=value; }"
   ancestor_wrong=[
    ("wrong-parent-token","Core.java","super(provider);","super((Class<?>)null);",0),
    ("wrong-parent-string","Core.java","super(provider);","super((String)null);",0),
    ("lost-ancestor-value","Core.java","this.value = provider.id;","this.value = null;",0),
    ("virtual-field-read","Core.java","this.value = provider.id;","this.value = provider.id();",3)]
   for label,old,new,mode in (("wrong-parent-kind","kind=3","kind=2",0),("repeat-parent-effect","calls++","calls+=2",0),("lost-parent-alias","seen=value","seen=null",0),("lost-partial-receiver","last=this","last=null",2)):
    ancestor_wrong.append((label,"Base.java",parent,parent.replace(old,new),mode))
   for label,filename,anchor,replacement,mode in ancestor_wrong:
    text=texts[filename]
    if text.count(anchor)!=1:raise RuntimeError("Ancestor constructor wrong-program anchor changed: "+label)
    source=w/label/filename;source.parent.mkdir();source.write_text(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
    result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),"fixture.AncestorConstructorHarness","0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
    if result.returncode==0 or "owned ancestor constructor mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong ancestor constructor program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned constructor-overload-argument definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=25:raise RuntimeError("Constructor-overload-argument coverage or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=25);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Constructor-overload-argument source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if (wanted["major"],actual["major"])!=(52,69):raise RuntimeError("Constructor-overload-argument class versions changed")
   actual["major"]=wanted["major"]
   if actual!=wanted:raise RuntimeError("Constructor-overload-argument metadata changed beyond reported helpers and required Java release: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Constructor-overload-argument Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Constructor overload argument effects, casts, returned identity or failures changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen constructor-overload-argument input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":25,"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"wrong_programs":len(wrong)+len(ancestor_wrong),"dependency_provider":args.dependency_provider,"nominal_sibling":args.nominal_sibling,"ancestor_input":args.ancestor_input};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
