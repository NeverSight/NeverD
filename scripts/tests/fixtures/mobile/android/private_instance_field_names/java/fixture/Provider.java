package fixture;
public final class Provider<T> implements java.io.Serializable {
    private final Object anchorValue;
    private Callback<Object,Object> cachedValue;
    private volatile long storedBits;
    public static int __neverdInstanceField0;
    public static void __neverdInstanceField1() {}
    public Provider(Callback<Object,Object> initial, Object anchor, long bits) {
        Trace.created = this;
        Trace.hit(1); anchorValue = anchor;
        Trace.hit(2); cachedValue = initial;
        Trace.hit(3); storedBits = bits; Trace.hit(4);
    }
    public Callback<Object,Object> exchange(Callback<Object,Object> next, long bits) {
        Trace.hit(5); Callback<Object,Object> old = cachedValue; Trace.seen = old;
        Trace.hit(6); cachedValue = next;
        Trace.hit(7); storedBits = bits; Trace.hit(8);
        return old;
    }
    public Object invoke(Object value) { Trace.hit(9); return cachedValue.apply(value); }
    public Callback<Object,Object> peek() { return cachedValue; }
    public Object anchor() { return anchorValue; }
    public long bits() { return storedBits; }
}
