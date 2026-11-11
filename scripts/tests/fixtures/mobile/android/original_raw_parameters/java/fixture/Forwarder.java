package fixture;
public final class Forwarder implements Api {
    public final Sink sink;
    public Forwarder(Sink sink) { this.sink = sink; }
    @SuppressWarnings("rawtypes")
    public final void accept(RawBox box, Object tag) { Support.forward(sink, box, tag); }
    public RawBox<?> captured() { return sink.seen; }
}
