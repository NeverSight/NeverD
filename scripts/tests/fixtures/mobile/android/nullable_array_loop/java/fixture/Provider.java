package fixture;
public final class Provider {
    public static int visits;
    public static int failAt;
    public static RuntimeException failure;
    public static void fill(String[] values) {
        if (values != null)
            for (int i = 0; i < values.length; i++) values[i] = i % 2 == 0 ? "alpha" : null;
    }
    public static String wrap(String value) {
        visits++;
        if (visits == failAt) throw failure;
        return value;
    }
}
