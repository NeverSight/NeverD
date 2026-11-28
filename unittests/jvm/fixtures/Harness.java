public final class Harness {
    public static void main(String[] ignored) {
        int[] inputs = {-7, 0, 1, 3, 8, 12, 19};
        for (int i = 0; i < inputs.length; i++) {
            int n = inputs[i];
            System.out.println(Sample.arithmetic(n, 5));
            System.out.println(Sample.distance(4000000000L + n, 19L));
            System.out.println(Sample.choose(n));
            System.out.println(Sample.sumTo(n));
            System.out.println(Sample.bucket(n));
            System.out.println(Sample.divide(20, n));
        }
    }
}
