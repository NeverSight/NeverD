package fixture;

public class CaptureHost {
    public static Base make(final int input, final Object token) {
        return new Base(3) {
            private final int extra = input + 3;

            @Override
            public int value() { return input; }
            @Override
            public Object token() { return token; }
            @Override
            public int extra() { return extra; }
        };
    }
}
