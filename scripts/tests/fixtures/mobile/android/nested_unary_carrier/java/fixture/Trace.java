package fixture;
public final class Trace {
 public static Point probe;
 public static boolean override;
 public static Object replacement;
 public static void hit(int phase,Object owner,Object first,Object second) {
  if(probe!=null)probe.visit(phase,owner,first,second);
 }
}
