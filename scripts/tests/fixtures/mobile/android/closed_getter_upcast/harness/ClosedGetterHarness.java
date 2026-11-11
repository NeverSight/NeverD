import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ClosedGetterHarness {
 private static void require(boolean value) {
  if(!value) throw new AssertionError("owned closed getter upcast changed");
 }
 public static void main(String[] args) throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  int step=mode>=20?9:mode>=18?8:mode>=4?1+(mode-4)/2:0;
  for(int attempt=0;attempt<2;++attempt) {
   Object first=mode==1?null:new Object(),replacement=new Object(),again=new Object();
   Leaf<Object,Token> mapping=mode==3?null:new Leaf<Object,Token>(),replacementMap=new Leaf<Object,Token>();
   Provider<Object> receiver=mode==2?null:new Provider<Object>(first,mapping);
   Throwable failure=mode%2==1?new AssertionError("sentinel"):new IllegalStateException("sentinel");
   Trace.events=0;Trace.failStep=step;Trace.failure=failure;Trace.replacementFirst=replacement;Trace.replacementAgain=again;Trace.replacementMap=replacementMap;
   Trace.seen=null;Trace.firstReceiver=null;Trace.mappingReceiver=null;
   Object output=null;Throwable caught=null;
   try {
    if(route==0) output=ClosedGetterCaller.make(receiver);
    else if(route==1) output=Child.class.getConstructor(Provider.class).newInstance(receiver);
    else if(route==2) output=MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,Provider.class)).invoke(receiver);
    else output=new Child<Object>(receiver);
   }catch(InvocationTargetException e){caught=e.getCause();}catch(Throwable e){caught=e;}
   long expected=attempt==0?(step==8?8:89):0;int last=0;
   if(step!=8 && step!=9 && mode!=2) for(int s=1;s<=7;++s) {
    expected=expected*10+s;last=s;if(s==step)break;
   }
   require(Trace.events==expected);
   if(step==8 || step==9) {
    if(attempt==1)require(caught instanceof NoClassDefFoundError);
    else if(failure instanceof Error)require(caught==failure);
    else require(caught instanceof ExceptionInInitializerError && caught.getCause()==failure);
   }else if(mode==2)require(caught instanceof NullPointerException);
   else require(caught==(step==0?null:failure));
   require(Trace.firstReceiver==(last>=2?receiver:null));
   require(Trace.mappingReceiver==(last>=4?receiver:null));
   if(receiver!=null) {
    require(receiver.firstValue==(last>=4?again:last>=2?replacement:first));
    require(receiver.mappingValue==(last>=4?replacementMap:mapping));
   }
   require((Trace.seen!=null)==(last>=6));
   if(last>=6) {
    Parent<?> seen=(Parent<?>)Trace.seen;require(seen.first==first && ((Object)seen.mapping)==mapping);
    if(last==7)require(((Child<?>)seen).receiver==receiver);
   }
   require(output==(caught==null?Trace.seen:null));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+expected);
  }
 }
}
