package fixture;
public final class Wrapped extends RuntimeException {
 public Wrapped(Throwable cause) {
  super(cause);
  Trace.wrapper = this;
  Trace.hit(8);
 }
}
