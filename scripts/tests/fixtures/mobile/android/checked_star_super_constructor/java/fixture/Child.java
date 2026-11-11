package fixture;
public final class Child extends Base {
    static { Trace.step(2); }
    public final Object first;
    public final Object second;
    public Child(Object key, Object first, Object second) {
        this.first = first;
        this.second = second;
        super((Context.Key<?>) key);
        Trace.seenFinal = this.key;
        Trace.mask |= 4;
        Trace.step(5);
    }
    public Object left() { return first; }
    public Object right() { return second; }
}
