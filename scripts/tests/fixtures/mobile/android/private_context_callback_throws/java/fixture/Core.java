package fixture;
public class Core {
  private <T> T read(Context context, Carrier<T> callback) throws java.io.IOException {
    Trace.receiver = this;
    Trace.hit(2);
    try {
      T value = callback.read(context.input);
      Trace.hit(4);
      return value;
    } finally {
      Trace.finish();
    }
  }
  private static <T> T readStatic(Context context, Carrier<T> callback) throws java.io.IOException {
    Trace.receiver = null;
    Trace.hit(2);
    try {
      T value = callback.read(context.input);
      Trace.hit(4);
      return value;
    } finally {
      Trace.finish();
    }
  }
  public <Item> Item call(Context context, Carrier<Item> callback) throws java.io.IOException {
    Trace.hit(1);
    Item value = read(context, callback);
    Trace.hit(6);
    return value;
  }
  public static <Item> Item callStatic(Context context, Carrier<Item> callback) throws java.io.IOException {
    Trace.hit(1);
    Item value = readStatic(context, callback);
    Trace.hit(6);
    return value;
  }
}
