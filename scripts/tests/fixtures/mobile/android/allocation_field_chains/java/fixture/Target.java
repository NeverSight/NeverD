package fixture;
public class Target {
    public Object value;
    public int number;
    static {
        Trace.hit(1);
        Trace.holder.link = Trace.initial;
        Trace.holder.count = Trace.initialNumber;
        Trace.hit(2);
    }
    protected Target(Object value, int number) {
        Trace.seen = this;
        Trace.hit(3);
        this.value = value;
        this.number = number;
        Trace.hit(4);
        Trace.holder.link = Trace.replacement;
        Trace.holder.count = Trace.afterNumber;
        Trace.hit(5);
    }
}
