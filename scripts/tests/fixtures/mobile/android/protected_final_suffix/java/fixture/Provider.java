package fixture;
public class Provider {
    static { Trace.hit(6); }
    public static Object lookup(int which) {
        Object value = which == 1 ? Trace.first : Trace.second;
        Trace.hit(which);
        return value;
    }
    public static long stamp() {
        long value = Trace.stamp;
        Trace.hit(3);
        return value;
    }
}
