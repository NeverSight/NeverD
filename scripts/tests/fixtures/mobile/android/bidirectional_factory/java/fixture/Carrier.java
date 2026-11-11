package fixture;
public final class Carrier<T,V extends Bound> {
    public final Object forward, reverse;
    public Carrier(Object forward, Object reverse) { this.forward=forward; this.reverse=reverse; }
}
