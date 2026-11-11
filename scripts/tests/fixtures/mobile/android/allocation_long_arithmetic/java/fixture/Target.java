package fixture;
public final class Target {
    public final Pair pair;
    public Target(Pair pair) { Trace.hit(7); this.pair=pair; Trace.hit(8); }
}
