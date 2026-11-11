import fixture.*;
import java.lang.invoke.*;import java.lang.reflect.*;import java.util.*;
public final class ConstructorLoopAllocationHarness {
 static void require(boolean ok){if(!ok)throw new AssertionError("owned constructor loop allocation");}
 static Core make(int route,Iterable<?> input,long word)throws Throwable{
  if(route==0)return Client.make(input,word);if(route==1)return ConstructorLoopAllocationCaller.make(input,word);
  if(route==2){try{return(Core)Core.class.getConstructor(Iterable.class,long.class,boolean.class).newInstance(input,word,true);}catch(InvocationTargetException f){throw f.getCause();}}
  return(Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Iterable.class,long.class,boolean.class)).invokeExact(input,word,true);
 }
 public static void main(String[] args)throws Throwable{
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;attempt++){
   Object item=mode==2?null:new Object();Iterable<?> input=mode==1?null:Collections.singletonList(item);long word=mode==12?Long.MIN_VALUE:mode==13?Long.MAX_VALUE:0x1234567887654321L+attempt;
   Trace.partial=null;Trace.token=null;Trace.events=0;Trace.position=0;Trace.stepCalls=0;Trace.count=mode==0?0:3;Trace.failAt=mode>=3&&mode<=11?mode-2:0;Trace.failure=new RuntimeException("owned loop failure");
   Core result=null;Throwable caught=null;try{result=make(route,input,word);}catch(Throwable f){caught=f;}
   if(mode==1)require(caught instanceof NullPointerException);else if(Trace.failAt!=0)require(caught==Trace.failure);else require(caught==null && result==Trace.partial);
   String events=mode==0?"891314567":"891289128912891314567";
   String expectedText=mode==1?"89":events;
   if(Trace.failAt!=0){int at=events.indexOf((char)('0'+Trace.failAt));require(at>=0);expectedText=events.substring(0,at+1);}
   long expected=0;for(int i=0;i<expectedText.length();i++)expected=expected*10+(expectedText.charAt(i)-'0');require(Trace.events==expected);
   int steps=0;for(int i=0;i<expectedText.length();i++)if(expectedText.charAt(i)=='2' && i+1<expectedText.length())steps++;
   require(Trace.stepCalls==steps && Trace.position==steps);
   require(Trace.token!=null && Trace.initializations==1);
   Object tokenInput=(mode==0||mode==1||mode==3||mode==4||mode==10||mode==11)?input:item;
   require(Trace.token.input==(mode==10?null:tokenInput) && Trace.token.word==(mode==10?0:word));
   if(mode==1||mode==3||mode==4||mode==5||mode==6||mode==10||mode==11)require(Trace.partial==null);
   else require(Trace.partial!=null && Trace.partial.value==(mode==7?null:Trace.token) && Trace.partial.word==(mode==7||mode==8?0:word));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
  }
 }
}
