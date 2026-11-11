package fixture;
public final class Provider {
    private Provider() {}
    public static Handler select(){Trace.factoryCalls++;Trace.step(4);return Trace.selected;}
}
