import fixture.LintFixture;

public final class LintHarness {
    public static void main(String[] args) {
        int[] values = {-2147483648, -19, 0, 7, 2147483647};
        for (int initial : values) {
            LintFixture fixture = new LintFixture(initial);
            System.out.println("field:" + initial + "=" + fixture.value);
            for (int delta : values) {
                System.out.println("plus:" + initial + ":" + delta + "=" + fixture.plus(delta));
            }
            System.out.println("twice:" + initial + "=" + LintFixture.twice(initial));
        }
    }
}
