package fixture;
public final class Trace {
 public static int calls,slot,phase;
 public static boolean write;
 public static Object array,input,first,key,output;
 public static Point probe;
 public static void hit(int value) {
  phase=value;
  if(probe!=null)probe.visit(value);
 }
}
