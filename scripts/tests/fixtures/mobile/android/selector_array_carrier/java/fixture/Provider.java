package fixture;
public final class Provider {
 public static <T> Result<T> choose(Selector<? super T,? extends Key<?>>... selectors) {
  Trace.calls++;
  Trace.array=selectors;
  Trace.hit(1);
  Selector<? super T,? extends Key<?>> selector=selectors[Trace.slot];
  Trace.first=selector;
  Key<?> key=selector.select((T)Trace.input);
  Trace.key=key;
  Trace.hit(2);
  if(Trace.write)selectors[0]=null;
  Trace.hit(3);
  return (Result<T>)Trace.output;
 }
}
