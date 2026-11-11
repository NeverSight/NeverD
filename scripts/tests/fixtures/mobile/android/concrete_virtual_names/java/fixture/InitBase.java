package fixture;
public class InitBase {
    static { Trace.seed = 100; Trace.value = Trace.initialized; Trace.hit(9); }
}
