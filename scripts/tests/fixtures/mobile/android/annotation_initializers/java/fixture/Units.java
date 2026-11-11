package fixture;

@java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
public @interface Units {
    int Z_VALUE = 37;
    Companion COMPANION = Companion.INSTANCE;

    int value() default 37;

    public static final class Companion {
        public static final Companion INSTANCE = new Companion();
        public final int seen;

        private Companion() {
            ++Trace.count;
            seen = Trace.read();
        }

        public int read() { return seen; }
    }
}
