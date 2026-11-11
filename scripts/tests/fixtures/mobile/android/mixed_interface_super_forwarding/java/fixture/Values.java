package fixture;
public class Values implements Context, Input {
    public long seed;
    public Values(long value) { seed = value; }
    @Override public int offset() {
        Trace.contextOwner = this;
        Trace.hit(2);
        if (Trace.contextAction != null) Trace.contextAction.run();
        return (int) seed;
    }
    @Override public long wideOffset() {
        Trace.contextOwner = this;
        Trace.hit(2);
        if (Trace.contextAction != null) Trace.contextAction.run();
        return seed;
    }
    @Override public int measure(int value) {
        Trace.inputOwner = this;
        Trace.observed = value;
        Trace.hit(4);
        if (Trace.inputAction != null) Trace.inputAction.run();
        return value ^ (int) seed;
    }
    @Override public long measure(long value) {
        Trace.inputOwner = this;
        Trace.observed = value;
        Trace.hit(4);
        if (Trace.inputAction != null) Trace.inputAction.run();
        return value ^ seed;
    }
}
