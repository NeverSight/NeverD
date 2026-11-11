package fixture;
public final class Trace {
 public static int events,failAt,limit;
 public static RuntimeException failure;
 public static Object values,target,callback,head,element,text,result,after;
 public static void hit(int event) { events=events*10+event;if(failAt==event)throw failure; }
}
