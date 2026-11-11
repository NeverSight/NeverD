package fixture;
public final class Trace {
 public static int events,failAt;
 public static RuntimeException failure;
 public static boolean mutate;
 public static Object reply,replacement,partial,element,after;
 public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
