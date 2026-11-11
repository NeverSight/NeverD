package fixture;

public class CaptureHost {
    public final Base product;

    public CaptureHost(final int input, final Object token) {
        super();
        product = new Base(3) {
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
