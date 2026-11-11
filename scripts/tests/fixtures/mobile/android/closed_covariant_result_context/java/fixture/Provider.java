package fixture;

public final class Provider {
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <T> T read(Object receiver, Class<? extends java.lang.annotation.Annotation> type, Object... values) throws IllegalArgumentException, IllegalStateException {
        Trace.calls++;
        Trace.stage = 1;
        Trace.receiver = receiver;
        Trace.type = type;
        Trace.values = values;
        Object first = values[0];
        Trace.first = first;
        Trace.stage = 2;
        Trace.action.run();
        Trace.stage = 3;
        return (T)Trace.value;
    }
    public static <T> T readFixed(Object receiver, Class<? extends java.lang.annotation.Annotation> type, Object[] values) throws IllegalArgumentException, IllegalStateException {
        Trace.relays++;
        return read(receiver, type, values);
    }
}
