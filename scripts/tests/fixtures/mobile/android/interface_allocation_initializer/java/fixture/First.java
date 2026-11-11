package fixture;
public final class First implements Api {
    static { Trace.hit(1, null); }
    public First() { Trace.hit(2, this); }
    public int id() { return 1; }
}
