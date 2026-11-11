package fixture;
public final class Target {
    static { Trace.hit(1); }
    public final Object[] array;
    public final Object stored;
    public final Object tag;
    public Target(Object[] array, Object stored, Object tag) {
        this.array = array; this.stored = stored; this.tag = tag;
        Trace.target = this; Trace.hit(5);
    }
}
