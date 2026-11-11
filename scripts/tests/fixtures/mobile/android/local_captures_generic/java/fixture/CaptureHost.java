package fixture;

public class CaptureHost {
    public static <T> Base make(final int input, final T token) {
        return new Base(3) {
            private final int extra = input + 3;

            @Override
            public int value() { return input; }
            @Override
            public T token() { return token; }
            @Override
            public int extra() { return extra; }
        };
    }
}
