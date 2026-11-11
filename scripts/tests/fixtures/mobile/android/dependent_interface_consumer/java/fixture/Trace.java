package fixture;
public final class Trace {
 public static Point probe;
 public static void record(int phase,Object value,Object mapper,Object context,Object result){probe.visit(phase,value,mapper,context,result);}
}
