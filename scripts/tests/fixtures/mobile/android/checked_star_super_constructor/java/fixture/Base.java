package fixture;
public abstract class Base implements Context.Element {
    static { Trace.step(1); }
    public final Context.Key<?> key;
    protected Base(Context.Key<?> key) {
        Trace.seenThis = this;
        Trace.seenKey = key;
        Trace.seenLeft = left();
        Trace.seenRight = right();
        Trace.mask |= 1;
        Trace.step(3);
        this.key = key;
        Trace.mask |= 2;
        Trace.step(4);
    }
    protected Base(Object value) {
        this.key = null;
        Trace.wrong = true;
        Trace.step(9);
    }
    public abstract Object left();
    public abstract Object right();
}
