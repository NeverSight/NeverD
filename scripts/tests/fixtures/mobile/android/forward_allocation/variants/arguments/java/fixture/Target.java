package fixture;
public final class Target {
    static { Trace.hit(1); }
    public Object first;
    public Object[] arguments;
    public String text;
    public Target(Object[] values, String message) {
        Trace.partial = this;
        arguments = values;
        text = message;
        first = Trace.value;
        Trace.hit(2);
    }
}
