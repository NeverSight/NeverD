package fixture;
public abstract class Provider {
    public int calls;
    static { Trace.hit(1); }
    public Provider() { Trace.hit(2); }
    public abstract <I, O> void accept(int flags, Pair<I, O> pair, I input, Object token, long bits);
}
