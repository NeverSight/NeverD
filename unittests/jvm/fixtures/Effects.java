public final class Effects {
    public static int seen;
    public static int ordered(int a, int b) {
        seen = a;
        int result = 12 / b;
        seen = result + a;
        return result;
    }
}
