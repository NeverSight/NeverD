package fixture;
public interface Root<T> {
    default T echo(T value) {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.firstFailure != null) throw Trace.firstFailure;
        return value;
    }
    private int hidden() {
        Trace.events = Trace.events * 10 + 2;
        if (Trace.secondFailure != null) throw Trace.secondFailure;
        return 17;
    }
    default int scalar() {
        int result = hidden();
        Trace.events = Trace.events * 10 + 3;
        return result;
    }
    static Object identity(Object value) {
        Trace.events = Trace.events * 10 + 4;
        if (Trace.boxFailure != null) throw Trace.boxFailure;
        return value;
    }
}
