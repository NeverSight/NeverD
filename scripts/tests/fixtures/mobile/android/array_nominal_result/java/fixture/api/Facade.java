package fixture.api;
import fixture.Trace;
public final class Facade extends Backend {
    static { Trace.hit(9, null); }
    private Facade() {}
}
