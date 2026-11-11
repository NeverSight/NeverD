package fixture;
public final class Provider {
    @SuppressWarnings("unchecked")
    public static <S,T> Carrier<T> create(Carrier<S> parent, T first, T second, String label, int mode) {
        Trace.hit(1);
        Trace.parent = parent;
        Trace.extracted = parent.first();
        Trace.label = label;
        Trace.hit(2);
        if (mode == 1) return (Carrier<T>)Trace.other;
        if (mode == 2) return null;
        if (mode == 3) return (Carrier<T>)(Carrier<?>)parent;
        Carrier<T> result = new Carrier<T>(first, second);
        Trace.created = result;
        Trace.hit(3);
        return result;
    }
}
