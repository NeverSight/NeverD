package fixture;
public final class Second implements Api {
    static { Trace.hit(3, null); }
    public Second() { Trace.hit(4, this); }
    public int id() { return 2; }
}
