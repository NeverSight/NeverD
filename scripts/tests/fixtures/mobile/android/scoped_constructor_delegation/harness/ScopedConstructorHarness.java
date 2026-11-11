import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ScopedConstructorHarness {
 private static void require(boolean value) {
  if(!value) throw new AssertionError("owned scoped constructor changed");
 }
 public static void main(String[] args) throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  Object first=mode==8?null:new Object(),second=mode==9?null:mode==12?first:new Object();
  Trace.seedFirst=first;Trace.seedSecond=second;
  boolean pair=mode==10 || mode==11;
  int step=mode>=14?9:mode>=2 && mode<=3?1:mode>=4 && mode<=5?2:mode>=6 && mode<=7?3:0;
  for(int attempt=0;attempt<2;++attempt) {
   Object value=mode==1?null:mode==13?first:new Object(),previous=mode==11?null:new Object();
   Object replacement=new Object();
   Throwable failure=mode%2==1?new AssertionError("sentinel"):new IllegalStateException("sentinel");
   Trace.events=0;Trace.failStep=step;Trace.failure=failure;Trace.replacement=replacement;Trace.seen=null;
   Object output=null;Throwable caught=null;
   try {
    if(route==0) output=ScopedConstructorCaller.make(value,previous,pair);
    else if(route==1) output=pair?Core.class.getConstructor(Object.class,Object.class).newInstance(value,previous):Core.class.getConstructor(Object.class).newInstance(value);
    else if(route==2) output=pair?MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Object.class,Object.class)).invoke(value,previous):MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Object.class)).invoke(value);
    else output=pair?new Core<Object>(value,previous):new Core<Object>(value);
   }catch(InvocationTargetException e){caught=e.getCause();}catch(Throwable e){caught=e;}
   long expected=attempt==0?9:0;boolean body=false;
   if(step!=9 && !(step==1 && attempt==1)) {
    if(attempt==0) expected=expected*10+1;
    if(step!=1) { expected=expected*10+2;if(step!=2) {body=true;expected=expected*10+3;} }
   }
   require(Trace.events==expected);
   if(step==9 || step==1) {
    if(attempt==1) require(caught instanceof NoClassDefFoundError);
    else if(failure instanceof Error) require(caught==failure);
    else require(caught instanceof ExceptionInInitializerError && caught.getCause()==failure);
   }else require(caught==(step==0?null:failure));
   require((Trace.seen!=null)==body);
   if(body) {
    Core<?> seen=(Core<?>)Trace.seen;
    require(seen.value==value && seen.previous==(pair?previous:first) && seen.next==second);
    require(Provider.first==replacement && Provider.second==replacement);
    first=replacement;second=replacement;
   }
   require(output==(caught==null?Trace.seen:null));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+expected);
  }
 }
}
