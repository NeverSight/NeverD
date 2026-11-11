package fixture;

public final class CheckedTransform implements Transform<Object, String> {
    public static int calls;
    public CheckedTransform() {}
    public final String apply(Object input) {
        String value = (String) input;
        calls++;
        return value;
    }
}
