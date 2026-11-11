package fixture;
public final class Child extends Base implements Tag<Object> {
    static { Trace.step(4); }
    public final Object capture;
    public int marker;
    @SuppressWarnings("unchecked")
    public Child(Object captured, Sink<? super Child> completion, long bits) {
        this.capture = captured;
        Trace.step(7);
        super(2, (Sink<Object>) (Sink<?>) completion, bits);
        marker = 99;
        Trace.step(5);
    }
}
