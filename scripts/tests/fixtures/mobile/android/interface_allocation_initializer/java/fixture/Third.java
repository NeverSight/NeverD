package fixture;
public final class Third implements Api {
    static { Trace.hit(5, null); }
    public Third() { Trace.hit(6, this); }
    public int id() { return 3; }
}
