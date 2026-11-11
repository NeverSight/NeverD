package fixture;
public final class Provider {
    static { Trace.hit(2); }
    public static float narrow(float value) { Trace.convertedFloat = value; Trace.calls++; Trace.hit(3); return value; }
    public static long wide(long value) { Trace.convertedLong = value; Trace.calls++; Trace.hit(3); return value; }
}
