package fixture;
public final class Core {
    public static int read(Bound value) {
        Trace.hit(1);
        int result = Trace.current.read(Trace.input(value));
        Trace.hit(8);
        return result;
    }
    public static int nested(Bound value) {
        Trace.hit(1);
        return Trace.finish(Trace.current.read(Trace.input(value)));
    }
}
