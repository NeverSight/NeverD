package fixture;

public class CaptureHost {
    public static Base make(final int input, final Object token) {
        return new Base(3) {
            private final Base nested = new Base(5) {
                @Override
                public int value() { return outerValue() + 1; }
                @Override
                public Object token() { return outerToken(); }
            };

            public int outerValue() { return input; }
            public Object outerToken() { return token; }
            @Override
            public int value() { return input; }
            @Override
            public Object token() { return token; }
            @Override
            public Base nested() { return nested; }
        };
    }
}
