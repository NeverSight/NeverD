package fixture;
public final class Trace {
 public static Point probe;
 public static void record(int phase,Object owner,Object item,int first,int second,String tag){
  if(probe!=null)probe.visit(phase,owner,item,first,second,tag);
 }
}
