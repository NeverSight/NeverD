package fixture;
public final class Provider {
    private static Callback<Object,Object> cachedValue;
    private static volatile long storedBits;
    public static int __neverdStaticField0;
    static {
        Trace.hit(1);cachedValue=Trace.initial;
        Trace.hit(2);storedBits=Trace.initialBits;Trace.hit(3);
    }
    public static Callback<Object,Object> exchange(Callback<Object,Object> next,long bits) {
        Trace.hit(4);Callback<Object,Object> old=cachedValue;Trace.seen=old;
        Trace.hit(5);cachedValue=next;
        Trace.hit(6);storedBits=bits;Trace.hit(7);
        return old;
    }
    public static Object invoke(Object value) { Trace.hit(8);return cachedValue.apply(value); }
    public static Callback<Object,Object> peek() { return cachedValue; }
    public static long bits() { return storedBits; }
}
