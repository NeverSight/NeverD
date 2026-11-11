package fixture;

public final class Provider {
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <T> T[] read(Object receiver, Class<?> type, Object... values) throws IllegalArgumentException, IllegalStateException {
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
        return (T[])Trace.value;
    }
    public static <T> T[] readFixed(Object receiver, Class<?> type, Object[] values) throws IllegalArgumentException, IllegalStateException {
        Trace.relays++;
        return read(receiver, type, values);
    }
    @SuppressWarnings("unchecked")
    public static <T> T[] allocate(Class<?> type, int size) {
        Trace.calls++;
        Trace.stage = 1;
        Trace.type = type;
        Trace.size = size;
        Object allocated = java.lang.reflect.Array.newInstance(type, size);
        Trace.allocated = allocated;
        Trace.stage = 2;
        Trace.action.run();
        Trace.stage = 3;
        return (T[])allocated;
    }
}
