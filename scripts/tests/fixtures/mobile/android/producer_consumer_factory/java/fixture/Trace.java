package fixture;
public final class Trace {
 public static Point probe;
 public static void hit(int phase,Object first,Object second) {
  if(probe!=null)probe.visit(phase,first,second);
 }
}
