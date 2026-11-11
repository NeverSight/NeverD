package fixture;
public final class Trace {
  private Trace() {}
  public static long events;
  public static int calls,failAt;
  public static Core seen;
  public static Object result;
  public static Runnable action;
  public static RuntimeException failure;
  public static void hit(int value){events=events*10+value;if(value==failAt)throw failure;}
}
