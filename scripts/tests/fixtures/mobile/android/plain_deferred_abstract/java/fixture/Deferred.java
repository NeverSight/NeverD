package fixture;
public abstract class Deferred extends AbstractWidget implements Api {
    public final Sink sink;
    public Deferred(Sink sink) { this.sink = sink; }
    @SuppressWarnings("rawtypes")
    public void accept(RawBox box, Object tag) { Support.forward(sink, box, tag); }
    public void ping(Object marker) { touch(marker); }
    public Object pull() { return item(); }
    public abstract <N> N identity(N value);
}
