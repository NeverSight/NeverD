package fixture;

public class Target {
    public final Object value;
    public final long number;

    static {
        Trace.hit(9);
        Trace.core.value = Trace.initialValue;
        Trace.core.number = Trace.number;
    }

    public Target(Object value, long number) {
        this.value = value;
        this.number = number;
        Trace.published = this;
        Trace.hit(8);
    }
}
