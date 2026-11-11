package fixture;

public final class Core {
    private Core() {}
    public static Object[] run(Object source, int length, Object type) {
        Object[] array = (Object[])source;
        Class token = (Class)type;
        Trace.hit(3, array, length, token, null);
        Object[] result = Provider.copy(array, length, token);
        Trace.hit(4, array, length, token, result);
        return result;
    }
    public static void ignore(Object source, int length, Object type) {
        Object[] array = (Object[])source;
        Class token = (Class)type;
        Trace.hit(3, array, length, token, null);
        Provider.copy(array, length, token);
        Trace.hit(4, array, length, token, null);
    }
}
