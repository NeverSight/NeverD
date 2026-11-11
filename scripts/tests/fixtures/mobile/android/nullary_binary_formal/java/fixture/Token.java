package fixture;
public final class Token implements Mirror {
    public Token() {}
    public Object apply(Object value) {
        Trace.applies++;
        Trace.seen=value;
        return Trace.applied;
    }
}
