package fixture;

public final class Defaults {
    public static Provider provider;
    static {
        Trace.hit(1);
        provider = (Provider) Trace.initialProvider;
    }
}
