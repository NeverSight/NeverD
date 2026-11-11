package fixture;
public final class Client {
  private Client() {}
  public static Object[] call(Object[][] rows) {
    Object[] result=Provider.select(rows);
    Trace.after=result;Trace.hit(7);return result;
  }
  public static String[] checked(Object[][] rows) {
    String[] result=(String[])Provider.select(rows);
    Trace.after=result;Trace.hit(7);return result;
  }
  public static String[] packed(String[] first,String[] second) {
    String[] result=Provider.select(first,second);
    Trace.after=result;Trace.hit(7);return result;
  }
}
