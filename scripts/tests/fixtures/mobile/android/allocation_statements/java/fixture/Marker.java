package fixture;
public class Marker {
    static { Trace.events = Trace.events * 10 + 2; if (Trace.failAt == 2) throw Trace.failure; }
    public static void before(int value, String label) {
        Trace.events = Trace.events * 10 + 3; Trace.calls++;
        if (value != Integer.MIN_VALUE || !label.equals("owned-\u03bb")) throw new AssertionError("original marker arguments");
        if (Trace.failAt == 3) throw Trace.failure;
    }
}
