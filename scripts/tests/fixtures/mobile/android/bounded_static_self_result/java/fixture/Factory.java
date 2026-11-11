package fixture;
public class Factory<T extends Bound> extends Parent.Base<T> implements Marker {
    @SuppressWarnings("unchecked")
    public static <T extends Bound> Factory<T> select(T seed) {
        Trace.hit(1); Trace.input = seed;
        Trace.hit(2); Trace.action.run(); Trace.hit(3);
        return (Factory<T>)Trace.result;
    }
}
