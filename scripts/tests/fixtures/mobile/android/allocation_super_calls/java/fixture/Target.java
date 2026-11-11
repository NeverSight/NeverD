package fixture;
public class Target {
    public Core owner;
    public Object value;
    public int number;
    static {
        Trace.hit(1);
        Trace.value = Trace.seed;
        Trace.number = Trace.initialNumber;
        Trace.hit(2);
    }
    public Target(Core source, Object input, int count) {
        Trace.seen = this;
        Trace.hit(5);
        owner = source; value = input; number = count;
        Trace.hit(6);
        Trace.value = Trace.replacement;
        Trace.number = Trace.afterNumber;
        Trace.hit(7);
    }
}
