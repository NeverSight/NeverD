package fixture;
public final class Provider {
    static { Trace.providerInit = true; Trace.step(2); }
    public static volatile Object value = Trace.payload;
}
