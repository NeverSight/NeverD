package fixture;
public class Facade extends Provider {
    static { Trace.facadeInitializations++; }
    private Facade() {}
}
