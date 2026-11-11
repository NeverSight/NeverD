import fixture.Client;
public final class NestedArrayResultCaller {
  public static Object[] call(Object[][] rows) { return Client.call(rows); }
  public static String[] checked(Object[][] rows) { return Client.checked(rows); }
  public static String[] packed(String[] first,String[] second) { return Client.packed(first,second); }
}
