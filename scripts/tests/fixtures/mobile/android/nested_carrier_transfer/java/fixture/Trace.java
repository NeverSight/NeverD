package fixture;
public final class Trace {
 public static Point probe;
 public static void record(int phase,Object input,int first,int second,Object output){probe.visit(phase,input,first,second,output);}
 public static Object choose(Object input,int first,int second){return probe.choose(input,first,second);}
}
