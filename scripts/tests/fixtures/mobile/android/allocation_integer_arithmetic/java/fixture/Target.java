package fixture;
public final class Target {
    static { Trace.event(1); Trace.x = 99; Trace.y = -71; Trace.delta = 13; }
    public final int left, top, right, bottom;
    public Target(int a, int b, int c, int d) {
        Trace.event(7);
        left = a; top = b; right = c; bottom = d;
    }
}
