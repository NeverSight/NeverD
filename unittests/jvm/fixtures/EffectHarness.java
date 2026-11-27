public final class EffectHarness {
    public static void main(String[] ignored) {
        int[] divisors = {2, 0, -3};
        for (int i = 0; i < divisors.length; i++) {
            try {
                System.out.println(Effects.ordered(7, divisors[i]));
            } catch (ArithmeticException ex) {
                System.out.println(ex.getClass().getName());
            }
            System.out.println(Effects.seen);
        }
    }
}
