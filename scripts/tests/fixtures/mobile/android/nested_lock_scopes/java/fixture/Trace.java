package fixture;
public final class Trace {
 public static int events,failAt,value;
 public static RuntimeException failure;
 public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
