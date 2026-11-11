import fixture.Counter;
import java.util.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ScalarBridgeHarness {
 private static final String[] NAMES={"byteValue","shortValue","intValue","longValue","floatValue","doubleValue","toByte","toShort","toInt","toLong","toFloat","toDouble"};
 private static final Class<?>[] TYPES={byte.class,short.class,int.class,long.class,float.class,double.class,byte.class,short.class,int.class,long.class,float.class,double.class};
 private static final int[] VALUES={Integer.MIN_VALUE,Integer.MAX_VALUE,-65537,-32769,-129,-128,-1,0,1,127,128,255,256,32767,32768,65535,16777215,16777216,16777217};
 private static void require(boolean value) {if(!value)throw new AssertionError("owned scalar number bridges changed");}
 private static long oracle(int value,Class<?> type) {
  if(type==byte.class)return (byte)value;if(type==short.class)return (short)value;
  if(type==float.class)return Float.floatToRawIntBits((float)value);
  if(type==double.class)return Double.doubleToRawLongBits((double)value);
  return value;
 }
 public static void main(String[] args) throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean projected=Boolean.parseBoolean(args[2]);
  require(Counter.class.getSuperclass()==AtomicInteger.class && Counter.class.getTypeParameters().length==0 && Modifier.isFinal(Counter.class.getModifiers()));
  require(Modifier.isPublic(Counter.class.getDeclaredConstructor(int.class).getModifiers()));
  for(int slot=0;slot<NAMES.length;++slot) {
   Method method=Counter.class.getDeclaredMethod(NAMES[slot]);
   require(method.getReturnType()==TYPES[slot] && method.getParameterTypes().length==0);
   require(Modifier.isPublic(method.getModifiers()) && Modifier.isFinal(method.getModifiers())==(slot<6));
   require(method.isBridge()==(!projected && (slot<6 || slot>=8)) && !method.isSynthetic());
  }
  int base=mode==VALUES.length?0:VALUES[mode];Counter receiver=mode==VALUES.length?null:new Counter(base);
  for(int attempt=0;attempt<2;++attempt) {
   for(int slot=0;slot<NAMES.length;++slot) {
    int expected=(attempt==0?base:base^0x6d5a56e9)+slot*31;
    if(receiver!=null)receiver.set(expected);
    Long actual=null;Throwable caught=null;
    try {
     if(route==0)actual=ScalarBridgeCaller.call(receiver,slot,NAMES[slot],TYPES[slot]);
     else if(route==1) {
      Number number=receiver;
      switch(slot) {
       case 0:actual=(long)number.byteValue();break;case 1:actual=(long)number.shortValue();break;
       case 2:actual=(long)number.intValue();break;case 3:actual=number.longValue();break;
       case 4:actual=(long)Float.floatToRawIntBits(number.floatValue());break;
       case 5:actual=Double.doubleToRawLongBits(number.doubleValue());break;
       case 6:actual=(long)receiver.toByte();break;case 7:actual=(long)receiver.toShort();break;
       default:actual=ScalarBridgeCaller.bits(Counter.class.getMethod(NAMES[slot]).invoke(receiver),TYPES[slot]);
      }
     }else if(route==2)actual=ScalarBridgeCaller.bits(Counter.class.getDeclaredMethod(NAMES[slot]).invoke(receiver),TYPES[slot]);
     else actual=ScalarBridgeCaller.bits(MethodHandles.publicLookup().findVirtual(Counter.class,NAMES[slot],MethodType.methodType(TYPES[slot])).invoke(receiver),TYPES[slot]);
    }catch(InvocationTargetException error){caught=error.getCause();}catch(Throwable error){caught=error;}
    if(receiver==null)require(caught instanceof NullPointerException && actual==null);
    else require(caught==null && actual!=null && actual.longValue()==oracle(expected,TYPES[slot]) && receiver.get()==expected);
   }
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",operations=12");
  }
 }
}
