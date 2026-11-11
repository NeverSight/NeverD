package fixture;

public class CaptureHost {
    public static <T> Base make(final int input, final T token) {
        return new Base(3) {
            @Override
            public int value() { return input; }
            @Override
            public T token() { return token; }
            @Override
            public Base nested() {
                final int captured = value();
                final T saved = token();
                return new Base(5) {
                    @Override
                    public int value() { return captured + 1; }
                    @Override
                    public T token() { return saved; }
                };
            }
        };
    }
}
