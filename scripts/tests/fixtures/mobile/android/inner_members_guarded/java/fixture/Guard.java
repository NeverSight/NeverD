package fixture;

public class Guard {
    public static int trace;
    private Guard() {}
    public static void check(int value, String stage) {
        boolean first = stage == "first";
        trace = trace * 10 + (first ? 1 : 2);
        if ((first && value < 0) || (!first && value == 0))
            throw new IllegalArgumentException(stage);
    }
}
