package fixture;
public abstract class GenericBase<O, R> {
    public final Class<R> type;
    public final String name;
    static { Trace.hit(1); }
    public GenericBase(Class<R> type, String name) {
        this.type = type; this.name = name;
        Trace.constructed = this;
        Trace.hit(3);
    }
    public abstract R get(O owner);
    public abstract void set(O owner, R value);
}
