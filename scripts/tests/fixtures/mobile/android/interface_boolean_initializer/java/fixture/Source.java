package fixture;
public class Source {
    static { Trace.hit(1); }
    public static int left() {
        Trace.defaultSeen=Policy.enabled;
        Trace.hit(2);
        return Trace.left;
    }
    public static int right() {
        Trace.left=Trace.late;
        Trace.hit(3);
        return Trace.right;
    }
}
