package fixture;
public class Factory<S> extends Base<S> {
    public static <T> Factory<T> select(Seed<T> seed, int code, Context first) {
        return select(seed, code, first, null);
    }
    @SuppressWarnings("unchecked")
    public static <T> Factory<T> select(Seed<T> seed, int code, Context first, Context second) {
        Trace.hit(1); Trace.input = seed; Trace.code = code; Trace.first = first; Trace.second = second;
        Trace.hit(2); Trace.action.run(); Trace.hit(3);
        return (Factory<T>)Trace.result;
    }
}
