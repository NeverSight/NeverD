package fixture;
public final class Provider {
    @SuppressWarnings("unchecked")
    public static <T,V extends Bound> Carrier<T,V> create(Callback<? super T,? extends V> forward,
                                                       Callback<? super V,? extends T> reverse) {
        Trace.hit(1);
        Trace.forward=forward; Trace.reverse=reverse;
        Trace.hit(2);
        Trace.observe(forward,reverse);
        Trace.hit(6);
        return (Carrier<T,V>)Trace.result;
    }
}
