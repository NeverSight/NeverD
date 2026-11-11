package fixture;
public final class Core {
 private Core() {}
 public static Carrier direct(Factory provider, String label, java.io.OutputStream sink) throws java.io.IOException {
  Trace.hit(4);
  Carrier value = provider.make(label, sink);
  Trace.hit(5);
  return value;
 }
 public static Carrier wrapped(Factory provider, String label, java.io.OutputStream sink) {
  try { return direct(provider, label, sink); }
  catch (java.io.IOException failure) { Trace.hit(6); throw new Wrapped(failure); }
 }
 public static <R extends Carrier<?>> R contextual(Factory provider, String label, java.io.OutputStream sink) throws java.io.IOException {
  Trace.hit(4);
  R value = (R) provider.from(label);
  Trace.hit(5);
  return value;
 }
}
