package fixture;

public class CaptureHost {
    public static Base make(final int input, final Object token) {
        return new Base(3) {
            private static int offset(int value) { return Offsets.add(value); }
            static final class Offsets {
                static int add(int value) { return value + 3; }
            }

            @Override
            public int value() { return input; }
            @Override
            public Object token() { return token; }
            @Override
            public int extra() { return offset(input); }
        };
    }
}
