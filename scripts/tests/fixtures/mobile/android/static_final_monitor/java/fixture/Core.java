package fixture;

public final class Core {
    public static final Object TOKEN = new Object();
    public static final float[] DATA = {1.0f, 2.5f, -0.0f, 9.0f};
    public static final int VALUE;

    static {
        synchronized (TOKEN) {
            Trace.inside(17, TOKEN, DATA);
        }
        Trace.cleanup(TOKEN);
        VALUE = Trace.finish(TOKEN);
    }

    private Core() {}
}
