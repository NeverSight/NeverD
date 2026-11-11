import fixture.*;
import java.io.IOException;
import java.io.OutputStream;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ClosedCovariantBoundHarness {
 private static void require(boolean value) { if (!value) throw new AssertionError("owned closed covariant bound mismatch"); }
 private static final class Payload extends Carrier<Element> {
  Payload(Object identity) { super(identity); }
  public void write(int value) { throw new AssertionError("owned closed covariant bound mismatch"); }
 }
 private static final class Sink extends OutputStream {
  int writes, value;
  public void write(int v) { ++writes; value = v; }
 }
 private static final class Alternate extends Factory {
  public <R extends Carrier<? extends Element>> R make(String label, OutputStream sink) throws IOException {
   Trace.receiver = this; Trace.label = label; Trace.sink = sink;
   Trace.hit(7); return (R) Trace.alternate;
  }
 }
 private static Carrier call(int route, int operation, Factory provider, String label, OutputStream sink) throws Throwable {
  if (route == 0) return operation == 0 ? Core.direct(provider, label, sink) : operation == 1 ? Core.wrapped(provider, label, sink) : Core.contextual(provider, label, sink);
  if (route == 1) return ClosedCovariantBoundCaller.call(operation, provider, label, sink);
  String name = operation == 0 ? "direct" : operation == 1 ? "wrapped" : "contextual";
  if (route == 2) {
   try { return (Carrier) Core.class.getMethod(name, Factory.class, String.class, OutputStream.class).invoke(null, provider, label, sink); }
   catch (InvocationTargetException failure) { throw failure.getCause(); }
  }
  return (Carrier) MethodHandles.publicLookup().findStatic(Core.class, name, MethodType.methodType(Carrier.class, Factory.class, String.class, OutputStream.class)).invokeExact(provider, label, sink);
 }
 public static void main(String[] args) throws Throwable {
  final int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
  final IOException checked = new IOException("owned checked");
  final RuntimeException runtime = new IllegalStateException("owned runtime");
  final Error fatal = new Error("owned fatal");
  final String label = mode == 2 ? null : new String("owned label");
  final Sink target = new Sink();
  final OutputStream sink = mode == 3 ? null : target;
  final OutputStream effectiveSink = operation == 2 ? null : sink;
  final Factory provider = mode == 4 ? null : mode == 14 ? new Alternate() : new Factory();
  final Object identity = new Object();
  final Carrier first = new Payload(identity), alternate = new Payload(new Object());
  Trace.failure = new IllegalArgumentException("owned stage");
  Trace.failAt = mode == 9 ? 4 : mode == 10 ? 1 : mode == 11 ? 3 : mode == 12 ? 5 : mode == 16 && operation == 1 ? 8 : 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
   Trace.events = 0; Trace.receiver = Trace.label = Trace.sink = Trace.wrapper = null;
   Trace.result = mode == 1 ? null : first; Trace.alternate = alternate;
   target.writes = target.value = 0;
   Trace.action = mode == 8 ? null : new Action() {
    public void run(String actualLabel, OutputStream actualSink) throws IOException {
     Trace.hit(2);
     require(actualLabel == label && actualSink == effectiveSink && Trace.receiver == provider);
     if (mode == 13 || mode == 17) Trace.result = alternate;
     if (mode == 15) actualSink.write(91);
     if (mode == 5 || mode == 16 || mode == 17) throw checked;
     if (mode == 6) throw runtime;
     if (mode == 7) throw fatal;
    }
   };
   Carrier result = null; Throwable thrown = null;
   try { result = call(route, operation, provider, label, sink); }
   catch (Throwable failure) { thrown = failure; }
   boolean checkedExit = mode == 5 || mode == 16 || mode == 17;
   long expected = mode == 4 || mode == 9 ? 4 : mode == 8 || mode == 10 ? 41 : mode == 14 ? 475 : mode == 11 ? 4123 : checkedExit ? operation == 1 ? 41268 : 412 : mode == 6 || mode == 7 || mode == 15 && operation == 2 ? 412 : 41235;
   require(Trace.events == expected);
   boolean entered = mode != 4 && mode != 9;
   require(Trace.receiver == (entered ? provider : null));
   require(Trace.label == (entered ? label : null) && Trace.sink == (entered ? effectiveSink : null));
   require(target.writes == (mode == 15 && operation != 2 ? 1 : 0) && target.value == (mode == 15 && operation != 2 ? 91 : 0));
   require((Trace.wrapper != null) == (checkedExit && operation == 1));
   if (Trace.wrapper != null) require(((Wrapped) Trace.wrapper).getCause() == checked);
   Throwable exact = checkedExit ? operation != 1 ? checked : mode == 16 ? Trace.failure : (Throwable) Trace.wrapper : mode == 6 ? runtime : mode == 7 ? fatal : Trace.failAt != 0 ? Trace.failure : null;
   if (exact != null) require(thrown == exact && result == null);
   else if (mode == 4 || mode == 8 || mode == 15 && operation == 2) require(thrown instanceof NullPointerException && result == null);
   else require(thrown == null && result == (mode == 1 ? null : mode == 13 || mode == 14 ? alternate : first));
   require(first.identity == identity);
   System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
  }
 }
}
