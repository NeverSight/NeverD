package fixture;
final class Pool {
    public static Object value;
    static { Trace.step(1);value=Trace.seed;Trace.step(2); }
}
