package fixture;
public class Core {
    public static final Object first, second;
    public static final long stamp;
    public static final boolean complete;
    static {
        Object a = null, b = null;
        long prepared = 0;
        boolean success = false;
        try {
            a = Provider.lookup(1);
            b = Provider.lookup(2);
            prepared = Provider.stamp();
            success = true;
        } catch (IllegalArgumentException failure) {
            Trace.caught = failure;
            Trace.hit(4);
        } catch (IllegalStateException failure) {
            Trace.caught = failure;
            Trace.hit(5);
        }
        if (success) {
            first = a; second = b; stamp = prepared; complete = true;
        } else {
            first = null; second = null; stamp = 0; complete = false;
        }
        Trace.hit(7);
    }
    public static Object read() { Trace.hit(8); return first; }
}
