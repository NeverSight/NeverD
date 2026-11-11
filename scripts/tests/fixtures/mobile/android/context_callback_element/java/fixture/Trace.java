package fixture;
public final class Trace {
 public static int events,failAt;
 public static long flags;
 public static Object frame;
 public static RuntimeException failure;
 public static Object input,context,saved,reply,after;
 public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
