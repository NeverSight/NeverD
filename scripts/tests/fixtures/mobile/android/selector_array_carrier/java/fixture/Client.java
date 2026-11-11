package fixture;
public final class Client {
 public static Result<Object> send(Selector<? super Object,? extends Key<?>>[] selectors) {
  return Provider.choose(selectors);
 }
 public static Result<String> typed(Selector<? super String,? extends Key<?>>[] selectors) {
  return Provider.choose(selectors);
 }
}
