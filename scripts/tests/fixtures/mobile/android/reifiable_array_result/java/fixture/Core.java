package fixture;

public final class Core {
    private Core() {}
    public static Object[] raw(Object receiver, Class<?> type, Object[] values) {
        return Provider.read(receiver, type, values);
    }
    public static String[] narrow(Object receiver, Class<?> type, Object[] values) {
        return (String[])Provider.read(receiver, type, values);
    }
    public static void ignore(Object receiver, Class<?> type, Object[] values) {
        Provider.read(receiver, type, values);
    }
    public static <V> V[] relay(Object receiver, Class<?> type, Object[] values) {
        return Provider.read(receiver, type, values);
    }
    public static Object[] fixed(Object receiver, Class<?> type, Object[] values) {
        return Provider.readFixed(receiver, type, values);
    }
    public static Object[] allocate(Class<?> type, int size) {
        return Provider.allocate(type, size);
    }
    public static String[] narrowAllocated(Class<?> type, int size) {
        return (String[])Provider.allocate(type, size);
    }
    public static void ignoreAllocated(Class<?> type, int size) {
        Provider.allocate(type, size);
    }
}
