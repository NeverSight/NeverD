package fixture;
public class Provider {
    static { Trace.step(2); }
    public static Provider INSTANCE = new Provider();
    public static Input check(Input value) { Trace.step(3); if (value == null) throw new NullPointerException(); return value; }
    public float first(Input value) {
        Trace.step(4); value.calls++; value.first = this; INSTANCE = new Provider(); return value.a;
    }
    public float second(Input value) { Trace.step(5); value.calls++; value.later = this; return value.b; }
    public float third(Input value) { Trace.step(6); value.calls++; return value.c; }
    public int edge(Input value) { Trace.step(7); value.calls++; return value.edge; }
}
