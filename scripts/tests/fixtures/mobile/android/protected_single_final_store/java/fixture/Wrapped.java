package fixture;
public class Wrapped extends RuntimeException {
 public Wrapped(Throwable cause) {
  super(cause);
  Trace.wrapper = this;
  Trace.hit(7);
 }
}
