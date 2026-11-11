import fixture.*;
import java.lang.invoke.*;import java.lang.reflect.*;import java.util.*;
public final class ConstructorLoopPrefixHarness {
 static void require(boolean ok){if(!ok)throw new AssertionError("owned constructor loop prefix");}
 static Core make(int route,Iterable<?> input,long word)throws Throwable{
  if(route==0)return Client.make(input,word);if(route==1)return ConstructorLoopPrefixCaller.make(input,word);
  if(route==2){try{return(Core)Core.class.getConstructor(Iterable.class,long.class,boolean.class).newInstance(input,word,true);}catch(InvocationTargetException f){throw f.getCause();}}
  return(Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Iterable.class,long.class,boolean.class)).invokeExact(input,word,true);
 }
 public static void main(String[] args)throws Throwable{
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;attempt++){
   Object item=mode==2?null:new Object();Iterable<?> input=mode==1?null:Collections.singletonList(item);long word=mode==10?Long.MIN_VALUE:mode==11?Long.MAX_VALUE:0x1234567887654321L+attempt;
   Trace.partial=null;Trace.events=0;Trace.position=0;Trace.stepCalls=0;Trace.last=null;Trace.count=mode==0?0:3;Trace.failAt=mode>=3&&mode<=9?mode-2:0;Trace.failure=new RuntimeException("owned loop failure");
   Core result=null;Throwable caught=null;try{result=make(route,input,word);}catch(Throwable f){caught=f;}
   if(mode==1)require(caught instanceof NullPointerException);else if(Trace.failAt!=0)require(caught==Trace.failure);else require(caught==null && result==Trace.partial);
   long full=mode==0?1314567L:1212121314567L;
   long expected=mode==1?0:mode==3?1:mode==4?12:mode==5?12121213L:mode==6?1212121314L:mode==7?12121213145L:mode==8?121212131456L:full;
   require(Trace.events==expected);
   int steps=mode==0||mode==1||mode==3||mode==4?0:3;require(Trace.stepCalls==steps && Trace.position==steps);
   if(mode<=6 && mode!=0 && mode!=2)require(Trace.partial==null);else require(Trace.partial!=null && Trace.partial.value==(mode==7?null:mode==0?input:item) && Trace.partial.word==(mode==7||mode==8?0:word));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
  }
 }
}
