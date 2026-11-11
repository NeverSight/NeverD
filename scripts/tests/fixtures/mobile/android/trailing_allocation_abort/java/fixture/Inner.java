package fixture;
public final class Inner {
    static { Trace.step(2); }
    public final Object value, style;
    public final float first, second;
    public Inner(Object value, float first, float second, Object style) {
        this.value = value; this.first = first; this.second = second; this.style = style;
        Trace.inner = this;
        Trace.step(5);
    }
}
