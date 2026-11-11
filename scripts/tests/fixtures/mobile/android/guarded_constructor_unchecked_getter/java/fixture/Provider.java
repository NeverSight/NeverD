package fixture;

public final class Provider {
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <T> T make(Class<T> type, Object... arguments) throws Failure {
        Trace.calls++;
        Trace.stage = 1;
        Trace.type = type;
        Trace.arguments = arguments;
        Trace.name = type.getName();
        Trace.arity = arguments.length;
        Trace.stage = 2;
        Trace.action.run();
        Trace.stage = 3;
        return (T)Trace.value;
    }
}
