import fixture.*;
import java.io.IOException;
import java.util.Map;
import java.util.HashMap;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ClosedContextResultHarness {
 private static void require(boolean value) { if (!value) throw new AssertionError("owned closed context result mismatch"); }
 private static final class Alternate extends Factory {
  public <R> R make(Map<String,Object> values) throws IOException {
   Trace.receiver = this; Trace.values = values; Trace.hit(7); return (R) Trace.alternate;
  }
 }
 private static Object call(int route, int operation, Factory provider, Map<String,Object> values) throws Throwable {
  if (route == 1) return ClosedContextResultCaller.call(operation, provider, values);
  Object result;
  if (route == 0) result = operation == 0 ? Core.direct(provider, values) : operation == 1 ? Core.wrapped(provider, values) : operation == 2 ? Core.contextual(provider, values) : operation == 3 ? Core.staticCall(provider, values) : Core.token(provider, values);
  else {
   String name = operation == 0 ? "direct" : operation == 1 ? "wrapped" : operation == 2 ? "contextual" : operation == 3 ? "staticCall" : "token";
   if (route == 2) {
    try { result = Core.class.getMethod(name, Factory.class, Map.class).invoke(null, provider, values); }
    catch (InvocationTargetException failure) { throw failure.getCause(); }
   } else result = MethodHandles.publicLookup().findStatic(Core.class, name, MethodType.methodType(Object.class, Factory.class, Map.class)).invokeExact(provider, values);
  }
  return operation == 2 ? (String) result : result;
 }
 public static void main(String[] args) throws Throwable {
  final int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
  final IOException checked = new IOException("owned checked");
  final RuntimeException runtime = new IllegalStateException("owned runtime");
  final Error fatal = new Error("owned fatal");
  final Map<String,Object> values = mode == 2 ? null : new HashMap<String,Object>();
  final Factory provider = mode == 3 ? null : mode == 9 ? new Alternate() : new Factory();
  final Object first = mode == 11 || mode == 12 || mode == 14 ? new Object() : new String("owned first");
  final Object alternate = new String("owned alternate"), token = new Object();
  Trace.failure = new IllegalArgumentException("owned stage");
  Trace.failAt = mode == 12 ? 5 : mode == 15 ? 3 : 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
   Trace.events = 0; Trace.receiver = Trace.values = Trace.wrapper = Trace.token = Trace.context = null;
   Trace.contextInput = mode == 2 ? null : token;
   Trace.factory = provider;
   Trace.result = mode == 1 ? null : first; Trace.alternate = alternate;
   if (values != null) { values.clear(); values.put("seed", token); }
   Trace.action = mode == 7 ? null : new Action() {
    public void run(Map<String,Object> actual) throws IOException {
     Trace.hit(2);
     require(actual == values && Trace.receiver == provider);
     if (actual != null) require(actual.get("seed") == token);
     if (mode == 8 || mode == 14) Trace.result = alternate;
     if (mode == 10 || mode == 13) actual.put("changed", token);
     if (mode == 4 || mode == 13) throw checked;
     if (mode == 5) throw runtime;
     if (mode == 6) throw fatal;
    }
   };
   Object result = null; Throwable thrown = null;
   try { result = call(route, operation, provider, values); }
   catch (Throwable failure) { thrown = failure; }
   boolean checkedExit = mode == 4 || mode == 13;
   long expected = mode == 3 ? 4 : mode == 7 ? 41 : mode == 9 ? 475 : checkedExit ? operation == 1 ? 41268 : 412 : mode == 5 || mode == 6 ? 412 : mode == 15 ? 4123 : 41235;
   if (operation == 4 && mode != 3) expected = Long.parseLong("49" + Long.toString(expected).substring(1));
   require(Trace.events == expected);
   require(Trace.token == (operation == 4 && mode != 3 ? String.class : null));
   require(Trace.context == (operation == 3 ? Trace.contextInput : null));
   require(Trace.receiver == (mode == 3 ? null : provider));
   require(Trace.values == (mode == 3 ? null : values));
   if (values != null) require(values.get("seed") == token && values.size() == (mode == 10 || mode == 13 ? 2 : 1) && (mode != 10 && mode != 13 || values.get("changed") == token));
   require((Trace.wrapper != null) == (checkedExit && operation == 1));
   if (Trace.wrapper != null) require(((Wrapped) Trace.wrapper).getCause() == checked);
   Throwable exact = checkedExit ? operation == 1 ? (Throwable) Trace.wrapper : checked : mode == 5 ? runtime : mode == 6 ? fatal : Trace.failAt != 0 ? Trace.failure : null;
   if (exact != null) require(thrown == exact && result == null);
   else if (mode == 3 || mode == 7) require(thrown instanceof NullPointerException && result == null);
   else if (mode == 11 && operation == 2) require(thrown instanceof ClassCastException && result == null);
   else require(thrown == null && result == (mode == 1 ? null : mode == 8 || mode == 9 || mode == 14 ? alternate : first));
   System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
  }
 }
}
