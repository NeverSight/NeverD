import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import kotlin.jvm.internal.DefaultConstructorMarker;

public final class ClosedRangeConstructorHarness {
 static final String MARKER="owned closed range constructor";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static int reads;static Object readReceiver;static Throwable readFailure;static boolean throwNull;
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 @SuppressWarnings("rawtypes") static final class Raw implements Range {
  final Comparable value;Raw(Comparable value){this.value=value;}
  public Comparable read(){reads++;readReceiver=this;if(throwNull)throw (RuntimeException)null;if(readFailure!=null)ClosedRangeConstructorHarness.<RuntimeException>fail(readFailure);return value;}
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Typed implements Range<Float> {
  final Float value;Typed(Float value){this.value=value;}
  public Float read(){reads++;readReceiver=this;return value;}
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 @SuppressWarnings("rawtypes") static Core create(int route,float value,Range range,int count,int mask,DefaultConstructorMarker marker)throws Throwable {
  if(route==0){try{return Core.class.getConstructor(float.class,Range.class,int.class,int.class,DefaultConstructorMarker.class).newInstance(value,range,count,mask,marker);}catch(InvocationTargetException error){throw error.getCause();}}
  MethodHandle ctor=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,float.class,Range.class,int.class,int.class,DefaultConstructorMarker.class));
  return(Core)ctor.invokeExact(value,range,count,mask,marker);
 }
 static DefaultConstructorMarker marker()throws Exception {
  Constructor<DefaultConstructorMarker> ctor=DefaultConstructorMarker.class.getDeclaredConstructor();ctor.setAccessible(true);return ctor.newInstance();
 }
 static void contract()throws Exception {
  Constructor<Core> primary=Core.class.getConstructor(float.class,Range.class,int.class);
  require(primary.getTypeParameters().length==0);
  ParameterizedType type=(ParameterizedType)primary.getGenericParameterTypes()[1];
  require(type.getRawType()==Range.class&&type.getActualTypeArguments()[0]==Float.class);
  TypeVariable<?> formal=Range.class.getTypeParameters()[0];
  ParameterizedType bound=(ParameterizedType)formal.getBounds()[0];
  require(bound.getRawType()==Comparable.class);
  WildcardType argument=(WildcardType)bound.getActualTypeArguments()[0];
  require(argument.getLowerBounds().length==1&&argument.getLowerBounds()[0]==formal);
 }
 @SuppressWarnings({"rawtypes","unchecked"}) public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);contract();
  final int[] values={0,0x80000000,0x7fc12345,0xffc00019,0x7f800000,0xff800000,1,0x7f7fffff,0x00800000,0xc1234567};
  for(int attempt=0;attempt<2;attempt++)for(final int mask:new int[]{0,4,-1,1,2,8,0x7ffffffb}){
   final int bits=values[mode%values.length];final float number=Float.intBitsToFloat(bits);final int count=attempt==0?31:Integer.MIN_VALUE;
   final int selected=(mask&4)!=0?7:count;
   Comparable expectedValue=mode==11?new String("raw-key"):mode==12?null:Float.valueOf(Float.intBitsToFloat(bits^0x80000000));
   final Range range=mode==10?null:mode==16?new Typed((Float)expectedValue):new Raw(expectedValue);
   final Throwable failure=mode==18?new AssertionError("failure"):mode==14||mode==19?new Checked():new IllegalStateException("failure");
   reads=0;readReceiver=null;readFailure=mode==13||mode==14?failure:null;throwNull=mode==15;
   Trace.calls=Trace.bits=Trace.count=0;Trace.receiver=Trace.range=null;
   Trace.probe=new Point(){public void visit(){
    require(reads==0&&Trace.calls==1&&Trace.receiver instanceof Core);
    Core partial=(Core)Trace.receiver;
    require(Float.floatToRawIntBits(partial.number)==bits&&partial.range==range&&partial.count==selected);
    require(Trace.bits==bits&&Trace.range==range&&Trace.count==selected);
    if(mode>=17)ClosedRangeConstructorHarness.<RuntimeException>fail(failure);
   }};
   Core actual=null;Float value=null;Throwable caught=null;
   try{actual=create(route,number,range,count,mask,attempt==0?null:marker());require(actual==Trace.receiver);if(range!=null)value=(Float)((Range)actual.range).read();}catch(Throwable error){caught=error;}
   require(Trace.calls==1&&Trace.receiver instanceof Core&&Trace.bits==bits&&Trace.range==range&&Trace.count==selected);
   Core partial=(Core)Trace.receiver;
   require(Float.floatToRawIntBits(partial.number)==bits&&partial.range==range&&partial.count==selected);
   boolean read=mode<17&&range!=null;require(reads==(read?1:0)&&readReceiver==(read?range:null));
   if(mode>=17||mode==13||mode==14)require(caught==failure);
   else if(mode==15)require(caught instanceof NullPointerException);
   else if(mode==11)require(caught instanceof ClassCastException);
   else require(caught==null&&actual==partial&&value==(range==null?null:expectedValue));
  }
  System.out.println("attempts=2,masks=7,closed-range-constructor=matched");
 }
}
