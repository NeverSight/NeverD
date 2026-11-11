package fixture;
public final class Child extends Base<CharSequence> implements Tag<Object> {
    static { Trace.step(4); }
    public final Object capture;
    public int marker;
    public Child(Object captured, Object context, int index, CharSequence[] input, long bits) {
        this.capture = captured;
        Trace.step(7);
        super(context, 2, index, input, bits);
        marker = 99;
        Trace.step(5);
    }
}
