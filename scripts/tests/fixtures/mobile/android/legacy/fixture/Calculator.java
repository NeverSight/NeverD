package fixture;

public class Calculator {
    public static int compute(int value) { return Nested.bump(Peer.twice(value)); }
    public static int sumAbs(int[] values) {
        int total = 0;
        for (int value : values) total += value < 0 ? -value : value;
        return total;
    }
    public static int safeDivide(int left, int right) {
        try { return left / right; }
        catch (ArithmeticException failure) { return -1; }
    }
    public static class Nested {
        public static int bump(int value) { return value + 3; }
    }
}
