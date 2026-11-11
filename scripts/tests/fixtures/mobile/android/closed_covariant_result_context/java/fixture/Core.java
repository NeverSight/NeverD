package fixture;

public final class Core {
    private Core() {}
    public static Object raw(Object receiver, Class<? extends java.lang.annotation.Annotation> type, Object[] values) {
        return Provider.read(receiver, type, values);
    }
    public static String narrow(Object receiver, Class<? extends java.lang.annotation.Annotation> type, Object[] values) {
        return (String)Provider.read(receiver, type, values);
    }
    public static void ignore(Object receiver, Class<? extends java.lang.annotation.Annotation> type, Object[] values) {
        Provider.read(receiver, type, values);
    }
    public static <V> V relay(V receiver, Class<? extends java.lang.annotation.Annotation> type, Object[] values) {
        return Provider.read(receiver, type, values);
    }
    public static Object fixed(Object receiver, Class<? extends java.lang.annotation.Annotation> type, Object[] values) {
        return Provider.readFixed(receiver, type, values);
    }
}
