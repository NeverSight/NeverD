package fixture;
public final class Trace {
 private Trace() {}
 public static long events;
 public static int failAt;
 public static RuntimeException failure;
 public static Object receiver, label, sink, wrapper;
 public static Carrier result, alternate;
 public static Action action;
 public static void hit(int event) {
  events = events * 10 + event;
  if (failAt == event) throw failure;
 }
}
