package fixture;
public final class Child extends Base {
    static { Trace.step(4); }
    public int marker;
    @SuppressWarnings("unchecked")
    public Child(Sink<? super Child> completion) {
        super((Sink<Object>) (Sink<?>) completion);
        marker = 99;
        Trace.step(5);
    }
}
