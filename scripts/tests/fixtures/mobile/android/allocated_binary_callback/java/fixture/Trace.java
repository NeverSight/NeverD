package fixture;
public final class Trace {
 public static long events; public static int mode; public static RuntimeException failure;
 public static Parent parent;public static Child child;public static Adapter adapter;
 public static Object result,after,seen,arg;
 public static void hit(int step){events=events*10+step;if(mode==step)throw failure;}
}
