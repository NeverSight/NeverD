package fixture;

public final class Core {
    private Core() {}
    public static Object raw(Object receiver, java.lang.reflect.Method method, Object[] values) {
        return Provider.read(receiver, method, values);
    }
    public static String narrow(Object receiver, java.lang.reflect.Method method, Object[] values) {
        return (String)Provider.read(receiver, method, values);
    }
    public static void ignore(Object receiver, java.lang.reflect.Method method, Object[] values) {
        Provider.read(receiver, method, values);
    }
    public static <V> V relay(V receiver, java.lang.reflect.Method method, Object[] values) {
        return Provider.read(receiver, method, values);
    }
    public static Object fixed(Object receiver, java.lang.reflect.Method method, Object[] values) {
        return Provider.readFixed(receiver, method, values);
    }
}
