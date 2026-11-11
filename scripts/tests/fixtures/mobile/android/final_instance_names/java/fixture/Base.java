package fixture;
public class Base {
    static { Trace.seed = 100; Trace.value = Trace.initialized; Trace.hit(9); }
}
