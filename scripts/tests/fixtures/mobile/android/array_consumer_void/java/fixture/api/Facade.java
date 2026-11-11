package fixture.api;
import fixture.Trace;
public class Facade extends Provider {
    static { Trace.hit(4); }
}
