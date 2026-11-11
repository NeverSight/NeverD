package fixture;
public final class Trace {
 public static Point probe;
 public static void hit(int phase,Object owner,Object[] arguments) {
  if(probe!=null)probe.visit(phase,owner,arguments);
 }
}
