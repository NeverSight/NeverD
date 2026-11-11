package fixture;
public final class Trace {
 public static Point probe;
 public static Core result;
 public static void hit(int phase,Core owner,char value) {
  if(probe!=null)probe.visit(phase,owner,value);
 }
}
