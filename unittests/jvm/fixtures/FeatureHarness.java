public final class FeatureHarness {
    public static void main(String[] args) {
        for (int n : new int[] { -7, 0, 42, Integer.MAX_VALUE }) {
            System.out.println(Features.add(n).applyAsInt(-3));
            System.out.println(Features.describe(n));
            System.out.println(Features.locked(n));
        }
    }
}
