package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Provider {
    static {
        Trace.step(9);
        if (Trace.initFatal != null) throw Trace.initFatal;
    }
    public static <T> Carrier<T> select(Carrier<? extends T> source,
            Comparison<? super T,? super T,Boolean> comparison) {
        Trace.step(1);
        T left = source.next();
        Trace.first = left;
        Trace.step(3);
        T right = source.next();
        Trace.second = right;
        Trace.step(5);
        Boolean accepted = comparison.apply(left, right);
        Trace.callbackResult = accepted;
        Trace.step(7);
        Trace.flag = accepted.booleanValue() ? 1 : 0;
        Trace.step(8);
        return (Carrier<T>)Trace.output;
    }
}
