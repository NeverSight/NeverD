package fixture;

public final class CheckedTransform implements Transform<Enum, String> {
    private final int bias;

    public CheckedTransform(int value) { bias = value; }

    public String apply(Enum input) {
        int value = bias;
        Flag flag = (Flag) input;
        return flag.text(value);
    }
}
