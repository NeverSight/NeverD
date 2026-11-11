package fixture;
public final class Trace {
 public static int calls,bits,count;
 public static Object receiver,range;
 public static Point probe;
 public static void record(Object object,float value,Object selected,int number) {
  calls++;receiver=object;bits=Float.floatToRawIntBits(value);range=selected;count=number;
  if(probe!=null)probe.visit();
 }
}
