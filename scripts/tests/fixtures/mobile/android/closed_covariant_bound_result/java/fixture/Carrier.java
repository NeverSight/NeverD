package fixture;
public abstract class Carrier<E extends Element> extends java.io.OutputStream {
 public final Object identity;
 protected Carrier(Object identity) { this.identity = identity; }
}
