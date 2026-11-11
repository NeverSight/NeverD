import fixture.*;import java.lang.reflect.*;import java.lang.invoke.*;
public final class CarrierAccessorNameHarness {
 static void require(boolean ok){if(!ok)throw new AssertionError("owned carrier accessor name");}
 static Object box(int route,float value)throws Throwable{
  if(route==0)return ValuesKt.boxed(value);if(route==1)return CarrierAccessorNameCaller.box(value);
  if(route==2){try{return ValuesKt.class.getMethod("boxed",float.class).invoke(null,value);}catch(InvocationTargetException f){throw f.getCause();}}
  return(Object)MethodHandles.publicLookup().findStatic(ValuesKt.class,"boxed",MethodType.methodType(Object.class,float.class)).invokeExact(value);
 }
 static int compare(int route,Object a,Object b)throws Throwable{
  if(route==0)return((Comparable)a).compareTo(b);if(route==1)return CarrierAccessorNameCaller.compare(a,b);
  if(route==2){try{return(Integer)Comparable.class.getMethod("compareTo",Object.class).invoke(a,b);}catch(InvocationTargetException f){throw f.getCause();}}
  return(int)MethodHandles.publicLookup().findVirtual(Comparable.class,"compareTo",MethodType.methodType(int.class,Object.class)).invokeExact((Comparable)a,b);
 }
 public static void main(String[]args)throws Throwable{int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);String getter=args[2];
  int[] bits={0x3fc00000,0xbfc00000,0x7fc12345,0x80000000,0x7f800000,0xff800000,1,0x7fffffff,0x3f800000,0x3f800000,0x3f800000,0x3f800000};
  for(int attempt=0;attempt<2;attempt++){
   float value=Float.intBitsToFloat(bits[mode]),other=Float.intBitsToFloat(attempt==0?bits[mode]:0x40000000);Trace.failAt=0;Object peer=ValuesKt.boxed(other);Trace.events=0;Trace.failAt=mode==9?1:mode==10?2:0;Trace.failure=new RuntimeException("owned scalar failure");
   Object boxed=null;Throwable failure=null;try{boxed=box(route,value);}catch(Throwable f){failure=f;}
   if(mode==9)require(failure==Trace.failure&&Trace.events==1&&boxed==null);else{
    require(failure==null&&boxed!=null);Method m=Scalar.class.getDeclaredMethod(getter);m.setAccessible(true);float actual=((Float)m.invoke(boxed)).floatValue();require(Float.floatToRawIntBits(actual)==Float.floatToRawIntBits(value));
    Object arg=mode==7?null:mode==8?new Object():peer;if(mode==11)boxed=null;int result=0;failure=null;try{result=compare(route,boxed,arg);}catch(Throwable f){failure=f;}
    if(mode==7||mode==11)require(failure instanceof NullPointerException);else if(mode==8)require(failure instanceof ClassCastException);else if(mode==10)require(failure==Trace.failure);else require(failure==null&&Integer.signum(result)==Integer.signum(Float.compare(value,other)));
    require(Trace.events==((mode==7||mode==8||mode==11)?1:12));
   }
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
  }
 }
}
