import fixture.*;import java.lang.invoke.*;import java.lang.reflect.*;
public final class NominalConstructorOverloadsHarness {
 static void require(boolean ok){if(!ok)throw new AssertionError("owned nominal constructor overloads");}
 static final class Actual implements Input {final Object token=new Object();public Object value(){return token;}}
 static Core<?> make(int route,float first,float second,Bound seed)throws Throwable{
  if(route==0)return Client.make(first,second,seed);if(route==1)return NominalConstructorOverloadsCaller.make(first,second,seed);
  if(route==2){try{return(Core<?>)Core.class.getConstructor(float.class,float.class,Bound.class).newInstance(first,second,seed);}catch(InvocationTargetException f){throw f.getCause();}}
  return(Core<?>)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,float.class,float.class,Bound.class)).invokeExact(first,second,seed);
 }
 public static void main(String[] args)throws Throwable{
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  Constructor<?> fixed=Core.class.getDeclaredConstructor(float.class,float.class,Input.class);require(Modifier.isPrivate(fixed.getModifiers()));
  Constructor<?> formal=Core.class.getConstructor(float.class,float.class,Bound.class);require(formal.getGenericParameterTypes()[2].equals(Core.class.getTypeParameters()[0]));
  int[] bits={0x7fc12345,0xffc54321,0x80000000,0,0x7f800000,0xff800000,0x7f7fffff,1,0x3f800001};
  for(int attempt=0;attempt<2;attempt++){
   Bound seed=mode==1?null:new Bound(Long.MIN_VALUE+attempt);Input expected=mode==2?null:new Actual();float first=Float.intBitsToFloat(bits[(mode+attempt)%bits.length]),second=Float.intBitsToFloat(bits[(mode+attempt+3)%bits.length]);
   Trace.partial=Trace.seed=null;Trace.output=expected;Trace.first=Trace.second=0;Trace.events=0;Trace.failAt=mode>=3&&mode<=6?mode-2:0;Trace.failure=new RuntimeException("owned overload failure");
   Core<?> result=null;Throwable caught=null;try{result=make(route,first,second,seed);}catch(Throwable t){caught=t;}
   long events=Trace.failAt==1?1:Trace.failAt==2?12:Trace.failAt==3?123:1234;require(Trace.events==events && Trace.seed==seed && Float.floatToRawIntBits(Trace.first)==Float.floatToRawIntBits(first) && Float.floatToRawIntBits(Trace.second)==Float.floatToRawIntBits(second));
   if(Trace.failAt!=0)require(caught==Trace.failure);else require(caught==null && result==Trace.partial);
   Core<?> partial=(Core<?>)Trace.partial;
   if(mode==3)require(partial==null);else require(partial!=null && partial.input==(mode==4?null:expected) && Float.floatToRawIntBits(partial.first)==((mode==4||mode==5)?0:Float.floatToRawIntBits(first)) && Float.floatToRawIntBits(partial.second)==((mode==4||mode==5)?0:Float.floatToRawIntBits(second)));
   if(result!=null && result.input!=null)require(result.input.value()==((Actual)expected).token);
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
  }
 }
}
