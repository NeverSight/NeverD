package fixture;
public final class Trace {
  private Trace() {}
  public static long events;
  public static Object result, receiver;
  public static Runnable action;
  public static java.io.IOException cleanup;
  public static RuntimeException post;
  public static void hit(int value) {
    events = events * 10 + value;
    if (value == 6 && post != null) throw post;
  }
  public static void finish() throws java.io.IOException {
    hit(5);
    if (action != null) action.run();
    if (cleanup != null) throw cleanup;
  }
}
