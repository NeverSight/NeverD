import fixture.Carrier;
import fixture.Core;
import fixture.Factory;
public final class ClosedCovariantBoundCaller {
 public static Carrier call(int operation, Factory provider, String label, java.io.OutputStream sink) throws Throwable {
  return operation == 0 ? Core.direct(provider, label, sink) : operation == 1 ? Core.wrapped(provider, label, sink) : Core.contextual(provider, label, sink);
 }
}
