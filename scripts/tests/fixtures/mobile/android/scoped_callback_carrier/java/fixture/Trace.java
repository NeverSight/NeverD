package fixture;
public final class Trace {
 public static Point probe;
 public static void record(int phase,Object owner,Object mapper,Object seed,int limit,Object result){
  if(probe!=null)probe.visit(phase,owner,mapper,seed,limit,result);
 }
}
