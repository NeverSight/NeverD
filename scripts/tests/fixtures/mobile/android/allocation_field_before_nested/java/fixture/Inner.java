package fixture;
public final class Inner {
    static { Trace.hit(3); Trace.holder.value = Trace.replacement; }
    public final Object value;
    public Inner(Object value) { Trace.hit(4); this.value = value; }
}
