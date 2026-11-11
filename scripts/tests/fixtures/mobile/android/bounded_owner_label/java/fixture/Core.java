package fixture;
public final class Core<T,V extends Bound> {
    public final T seed, threshold;
    public final Link<T,V> converter;
    public final String label;
    public Core(T seed, Link<T,V> converter, T threshold, String label) {
        Trace.partial=this;Trace.hit(1);
        this.seed=seed;Trace.hit(2);
        this.converter=converter;this.threshold=threshold;Trace.hit(3);
        this.label=label;Trace.hit(4);
    }
    @SuppressWarnings("unchecked")
    public Core(Object seed, Link converter, Object threshold) {
        this((T)seed,(Link<T,V>)converter,(T)threshold,"owned label");
    }
}
