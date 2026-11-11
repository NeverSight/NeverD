package fixture;
public final class Box<T> {
    static { Trace.hit(1); }
    public final long bits;
    public Box(long bits) { Trace.hit(2);this.bits=bits;Trace.box=this;Trace.hit(3); }
}
