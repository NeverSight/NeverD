package fixture;
public final class Core {
    static { Trace.step(1); }
    public final float x, y, z;
    public final int edge;
    public Core(float x, float y, float z, int edge) {
        Trace.step(8); this.x = x; this.y = y; this.z = z; this.edge = edge;
    }
    public Core(Input value) {
        this(Provider.INSTANCE.first(Provider.check(value)), Provider.INSTANCE.second(value),
             Provider.INSTANCE.third(value), Provider.INSTANCE.edge(value));
    }
}
