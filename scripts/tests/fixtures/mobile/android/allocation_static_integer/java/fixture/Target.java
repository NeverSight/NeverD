package fixture;
public final class Target {
    static { Trace.event(1); }
    public final Object owner;
    public final int number;
    public Target(Object value, int count) {
        Trace.event(7);
        Fields.number = Trace.afterConstructor;
        Trace.owner = Trace.replacement;
        owner = value;
        number = count;
    }
}
