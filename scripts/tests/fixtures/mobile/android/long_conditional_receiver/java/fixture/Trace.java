package fixture; public final class Trace {public static Point probe;public static Object created;public static void record(int phase,int index,Object value){probe.visit(phase,index,value);}}
