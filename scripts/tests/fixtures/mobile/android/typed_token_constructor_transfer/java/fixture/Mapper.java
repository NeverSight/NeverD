package fixture;
public final class Mapper {
    public static Context observer;
    public static Object checked, marker;
    public static String label;
    public static Input saved;
    public static Output replacement;
    public static Core escaping;
    public static RuntimeException failure, constructorFailure;
    private Mapper() {}
    public static void check(Object value, String name) {
        checked = value; label = name; observer.check(value, name);
        if (value == null) throw new NullPointerException(name);
    }
    public static <X> Output<X> map(Input<X> input) {
        observer.check(input, "map"); saved = input;
        marker = input.inspect();
        if (failure != null) throw failure;
        return replacement;
    }
}
