package fixture;
public final class Child extends Parent {
    public Fn<Object,Unit> zeroCallback;
    public long zeroBits;
    public boolean zeroFlag;
    public Object[] zeroArray;
    public final Object marker;
    public Child(Object token) {
        long bits=Trace.wide();
        Object selected=token!=null ? Trace.choice(token) : Trace.choice(Trace.fallback);
        super(bits,selected);
        Trace.hit(5);
        marker=selected;
    }
    public Unit read(Object input) {
        Trace.hit(6);
        return zeroCallback.apply(input);
    }
}
