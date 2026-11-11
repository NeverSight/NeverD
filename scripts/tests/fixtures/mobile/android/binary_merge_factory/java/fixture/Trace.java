package fixture;
public final class Trace {
 public static int calls;
 public static Object first,second,callback,output;
 public static Point probe;
 public static void record(Object a,Object b,Object merge) {
  calls++;first=a;second=b;callback=merge;
  if(probe!=null)probe.visit();
 }
}
