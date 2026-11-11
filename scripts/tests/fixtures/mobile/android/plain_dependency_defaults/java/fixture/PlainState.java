package fixture;

public class PlainState extends java.util.concurrent.atomic.AtomicBoolean {
    public PlainState(boolean initial) { super(initial); }
    public String toString() { return "owned parent"; }
}
