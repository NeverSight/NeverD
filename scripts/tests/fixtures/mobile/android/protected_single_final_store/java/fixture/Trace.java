package fixture;
public class Trace {
 public static long events;
 public static int failAt;
 public static Object partial, allocated, wrapper;
 public static java.io.IOException caught, checked;
 public static RuntimeException failure, runtime;
 public static Error fatal;
 public static Action valueAction, afterAction;
 public static void hit(int event) { events=events*10+event; if(event==failAt)throw failure; }
}
