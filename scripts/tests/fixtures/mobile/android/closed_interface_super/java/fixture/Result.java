package fixture;
public final class Result {
    public final Object receiver;
    public final Special item;
    public Result(Object receiver, Special item) {
        Trace.step(4);
        this.receiver = receiver; this.item = item;
        Trace.constructed = this;
        Trace.step(5);
    }
}
