package fixture;

public final class Core {
    private Core() {}
    public static Object local(Class<?> type) { return new Subject(type).value; }
    public static Object direct(Class<?> type) { return new Direct(type).value; }
    public static Object localArray(Class<?> type, Object[] arguments) { return new Subject(type, arguments).value; }
    public static Object directArray(Class<?> type, Object[] arguments) { return new Direct(type, arguments).value; }
    public static Object ignored(Class<?> type) { new Subject(type); return Trace.marker; }
    public static Object ignoredArray(Class<?> type, Object[] arguments) { new Direct(type, arguments); return Trace.marker; }
}
