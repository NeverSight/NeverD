package fixture;
public final class Trace {
  private Trace() {}
  public static int events,length,failAt;
  public static RuntimeException failure;
  public static Object[][] seen;
  public static Object row,element,result,after;
  public static Action action;
  public static void hit(int event) {
    events=events*10+event;
    if(event==failAt)throw failure;
  }
  public static Object[] apply(Object[][] rows) {
    length=rows.length;hit(3);
    rows[0]=(Object[])row;hit(4);
    rows[0][0]=element;hit(5);
    action.run();
    return (Object[])result;
  }
}
