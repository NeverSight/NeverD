package fixture;
public final class Core {
    public final float narrow;
    public final long wide;
    private Core(float value) {
        Trace.partial = this; Trace.hit(4);
        narrow = value; wide = 0; Trace.hit(5);
    }
    private Core(long value) {
        Trace.partial = this; Trace.hit(4);
        narrow = 0; wide = value; Trace.hit(5);
    }
    public Core(int seed, float incoming, int mask, Object marker) {
        this((mask & 1) != 0 ? Provider.narrow((float) Trace.readInt(seed)) : incoming);
    }
    public Core(double seed, long incoming, int mask, Object marker) {
        this((mask & 1) != 0 ? Provider.wide((long) Trace.readDouble(seed)) : incoming);
    }
}
