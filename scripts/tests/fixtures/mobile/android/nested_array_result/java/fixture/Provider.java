package fixture;
public final class Provider {
  private Provider() {}
  static { Trace.hit(1); }
  @SuppressWarnings("unchecked")
  public static <T> T[] select(T[]... rows) {
    Trace.seen=rows;Trace.hit(2);
    Object[] result=Trace.apply(rows);
    Trace.hit(6);
    return (T[])result;
  }
}
