package fixture;
public final class Core {
 private Core() {}
 public static Object direct(Factory provider, java.util.Map<String,Object> values) throws java.io.IOException {
  Trace.hit(4);
  Object value = provider.make(values);
  Trace.hit(5);
  return value;
 }
 public static Object wrapped(Factory provider, java.util.Map<String,Object> values) {
  try { return direct(provider, values); }
  catch (java.io.IOException failure) { Trace.hit(6); throw new Wrapped(failure); }
 }
 public static <Q> Q contextual(Factory provider, java.util.Map<String,Object> values) throws java.io.IOException {
  Trace.hit(4);
  Q value = provider.make(values);
  Trace.hit(5);
  return value;
 }
 public static Object staticCall(Factory provider, java.util.Map<String,Object> values) throws java.io.IOException {
  Trace.hit(4);
  Object value = Factory.create(values, Trace.contextInput);
  Trace.hit(5);
  return value;
 }
 public static Object token(Factory provider, java.util.Map<String,Object> values) throws java.io.IOException {
  Trace.hit(4);
  Object value = provider.token(String.class, values);
  Trace.hit(5);
  return value;
 }
}
