import fixture.*;import java.lang.invoke.*;import java.lang.reflect.*;
public final class AllocationRetainedCallHarness {
 static void require(boolean ok){if(!ok)throw new AssertionError("owned allocation retained call");}
 static Quad make(int route,float input)throws Throwable{
  if(route==0)return Client.make(input);if(route==1)return AllocationRetainedCallCaller.make(input);
  if(route==2){try{return(Quad)Core.class.getMethod("make",float.class).invoke(null,input);}catch(InvocationTargetException f){throw f.getCause();}}
  return(Quad)MethodHandles.publicLookup().findStatic(Core.class,"make",MethodType.methodType(Quad.class,float.class)).invokeExact(input);
 }
 public static void main(String[] args)throws Throwable{
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);int[] bits={0x7fc12345,0xffc54321,0x80000000,0,0x7f800000,0xff800000,0x7f7fffff,1,0x3f800001};
  for(int attempt=0;attempt<2;attempt++){
   float input=Float.intBitsToFloat(bits[(mode+attempt)%bits.length]);Object token=mode==1?null:new Object();Trace.token=token;Trace.events=0;Trace.observed=0;Trace.saved=Trace.partial=Trace.after=null;Trace.failAt=mode>=2&&mode<=5?mode-1:0;Trace.failure=new RuntimeException("owned retained failure");
   Quad result=null;Throwable caught=null;try{result=make(route,input);}catch(Throwable t){caught=t;}
   long tail=Trace.failAt==1?1:Trace.failAt==2?12:Trace.failAt==3?123:1234;long events=attempt==0?(tail==1?91:tail==12?912:tail==123?9123:91234):tail;
   require(Trace.events==events && Trace.initializations==1 && Float.floatToRawIntBits(Trace.observed)==Float.floatToRawIntBits(input));
   if(Trace.failAt!=0)require(caught==Trace.failure);else require(caught==null && result==Trace.partial && result==Trace.after);
   if(mode==2)require(Trace.partial==null);else{Quad partial=(Quad)Trace.partial;require(partial!=null);int expected=mode==3?0:Float.floatToRawIntBits(input);require(Float.floatToRawIntBits(partial.a)==expected && Float.floatToRawIntBits(partial.b)==expected && Float.floatToRawIntBits(partial.c)==expected && Float.floatToRawIntBits(partial.d)==expected);}
   require(Trace.saved==((mode==2||mode==3||mode==4)?null:token) && Trace.after==((mode==2||mode==3||mode==4)?null:Trace.partial));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
  }
 }
}
