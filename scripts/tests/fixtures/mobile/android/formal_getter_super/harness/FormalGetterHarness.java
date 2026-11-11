import fixture.*;
import java.util.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class FormalGetterHarness {
 private static void require(boolean value) {
  if(!value) throw new AssertionError("owned formal getter super changed");
 }
 public static void main(String[] args) throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  int step=mode>=16?9:mode>=14?8:mode>=4?1+(mode-4)/2:0;
  for(int attempt=0;attempt<2;++attempt) {
   Object key=mode==3?null:new Object(),input=mode==1?null:new Object(),replacement=new Object();
   Provider<Object> receiver=mode==2?null:new Provider<Object>(input);
   Map<Object,Provider<Object>> map=new HashMap<Object,Provider<Object>>();map.put(key,receiver);
   Throwable failure=mode%2==1?new AssertionError("sentinel"):new IllegalStateException("sentinel");
   Trace.events=0;Trace.failStep=step;Trace.failure=failure;Trace.replacement=replacement;Trace.changedKey=key;Trace.seen=null;Trace.seenProvider=null;
   Object output=null;Throwable caught=null;
   try {
    if(route==0) output=FormalGetterCaller.make(map,key,receiver);
    else if(route==1) output=Child.class.getConstructor(Map.class,Object.class,Provider.class).newInstance(map,key,receiver);
    else if(route==2) output=MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,Map.class,Object.class,Provider.class)).invoke(map,key,receiver);
    else output=new Child<Object,Object>(map,key,receiver);
   }catch(InvocationTargetException e){caught=e.getCause();}catch(Throwable e){caught=e;}
   long expected=attempt==0?(step==8?8:89):0;int last=0;
   if(step!=8 && step!=9 && mode!=2) for(int s=1;s<=5;++s) {
    expected=expected*10+s;last=s;if(s==step)break;
   }
   require(Trace.events==expected);
   if(step==8 || step==9) {
    if(attempt==1)require(caught instanceof NoClassDefFoundError);
    else if(failure instanceof Error)require(caught==failure);
    else require(caught instanceof ExceptionInInitializerError && caught.getCause()==failure);
   }else if(mode==2)require(caught instanceof NullPointerException);
   else require(caught==(step==0?null:failure));
   require(Trace.changedKey==(last>=2?replacement:key));
   require(Trace.seenProvider==(last>=2?receiver:null));
   if(receiver!=null)require(receiver.value==(last>=2?replacement:input));
   require((Trace.seen!=null)==(last>=4));
   if(last>=4) {
    Parent<?,?> seen=(Parent<?,?>)Trace.seen;require(seen.key==key && seen.value==input);
    if(last==5) {Child<?,?> child=(Child<?,?>)seen;require(((Object)child.map)==map && ((Object)child.receiver)==receiver);}
   }
   require(output==(caught==null?Trace.seen:null));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+expected);
  }
 }
}
