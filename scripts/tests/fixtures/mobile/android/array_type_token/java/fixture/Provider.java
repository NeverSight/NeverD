package fixture;

public final class Provider {
    private Provider() {}
    public static <T, U> T[] copy(U[] array, int length, Class<? extends T[]> token) {
        Trace.hit(1, array, length, token, null);
        T[] result = java.util.Arrays.copyOf(array, length, token);
        Trace.hit(2, array, length, token, result);
        return result;
    }
}
