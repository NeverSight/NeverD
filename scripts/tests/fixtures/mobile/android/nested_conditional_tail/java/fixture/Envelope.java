package fixture;
public final class Envelope {
    static { Trace.step(1); Trace.provider.payload = Trace.outerPayload; }
    public final Item item;
    public final long bits;
    public final Object extra;
    public Envelope(Item value, long number, Object data) {
        Trace.outer = this;
        Trace.step(8);
        item = value;
        Trace.step(9);
        bits = number;
        Trace.step(10);
        extra = data;
        Trace.step(11);
    }
}
