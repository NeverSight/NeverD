package fixture;

public class MarkerHost {
    private static final Callback CALLBACK = new Callback() {
        public int value(int input) { return input + 9; }
    };

    static class Box {
        int value;
        private Box(int input) { value = input; }
    }

    public static int read(int input) { return new Box(input).value; }
    public static int callback(int input) { return CALLBACK.value(input); }
}
