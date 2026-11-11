import fixture.Counter;
import java.lang.reflect.*;
public final class ScalarBridgeCaller {
 public static long bits(Object value,Class<?> type) {
  if(type==float.class)return Float.floatToRawIntBits(((Float)value).floatValue());
  if(type==double.class)return Double.doubleToRawLongBits(((Double)value).doubleValue());
  return ((Number)value).longValue();
 }
 public static long call(Counter value,int slot,String name,Class<?> type) throws Throwable {
  switch(slot) {
   case 0:return value.byteValue();case 1:return value.shortValue();
   case 2:return value.intValue();case 3:return value.longValue();
   case 4:return Float.floatToRawIntBits(value.floatValue());
   case 5:return Double.doubleToRawLongBits(value.doubleValue());
   case 6:return value.toByte();case 7:return value.toShort();
   default:try{return bits(Counter.class.getMethod(name).invoke(value),type);}
           catch(InvocationTargetException error){throw error.getCause();}
  }
 }
}
