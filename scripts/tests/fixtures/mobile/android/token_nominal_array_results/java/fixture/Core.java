package fixture;

public final class Core {
    private Core() {}

    public static Box<?>[] raw(Class<?> token) {
        return Provider.read(token);
    }

    public static Object[] objects(Class<?> token) {
        return Provider.read(token);
    }

    public static void ignore(Class<?> token) {
        Provider.read(token);
    }

    public static <U> Box<U>[] relay(Class<U> token) {
        return Provider.read(token);
    }

    public static String stringValue(Class<String> token) {
        return Provider.read(token)[0].value;
    }

    public static Box<?>[] overwrite(Class<?> token, Box<?> replacement) {
        Box<?>[] array = Provider.read(token);
        array[0] = replacement;
        return array;
    }
}
