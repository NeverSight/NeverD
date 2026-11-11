package fixture;

@SuppressWarnings({"unchecked", "rawtypes"})
public final class Core {
    private Core() {}
    public static Object[] raw(Object[] values, Gate<?> gate) {
        return Provider.apply(values, (Gate)gate);
    }
    public static String[] narrow(Object[] values, Gate<?> gate) {
        return (String[])Provider.apply(values, (Gate)gate);
    }
    public static void ignore(Object[] values, Gate<?> gate) {
        Provider.apply(values, (Gate)gate);
    }
    public static <E> E[] relay(E[] values, Gate<E> gate) {
        return Provider.apply(values, gate);
    }
    public static Object[] reverse(Object[] values, Gate<?> gate) {
        return Provider.reversed((Gate)gate, values);
    }
}
