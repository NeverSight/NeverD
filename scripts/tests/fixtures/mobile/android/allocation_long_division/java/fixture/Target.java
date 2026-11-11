package fixture;
public final class Target {
    static { Trace.initializations=Trace.initializations*10+1; }
    public final Pair pair;
    public Target(Pair pair) { Trace.hit(7); this.pair=pair; Trace.hit(8); }
}
