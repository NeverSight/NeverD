package fixture;
public final class Scalar {
    public final long decode() {
        Trace.step(1);
        Trace.seen = this;
        long value = Trace.bits;
        Trace.bits += 7;
        return value;
    }
}
