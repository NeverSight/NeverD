package fixture;
public final class PackageBoundScalarCaller {
    public static <T extends Bound> int read(T value) {
        Trace.hit(1);
        int result = Trace.current.read(Trace.input(value));
        Trace.hit(8);
        return result;
    }
}
