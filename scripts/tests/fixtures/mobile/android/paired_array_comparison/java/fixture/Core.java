package fixture;

public final class Core {
    private Core() {}

    public static boolean variable(Object[] left, Object[] right) {
        return Provider.variable(left, right);
    }

    public static boolean fixed(Object[] left, Object[] right) {
        return Provider.fixed(left, right);
    }

    public static Object ignored(Object[] left, Object[] right) {
        Provider.variable(left, right);
        return Trace.marker;
    }

    public static boolean typed(String[] left, String[] right) {
        return Provider.variable(left, right);
    }
}
