package fixture;

public class Core {
    public static Target create(Provider provider, Object input) {
        return new Target((long) provider.wide, (double) provider.narrow, (float) provider.number,
                (byte) provider.integer, (short) provider.integer, (char) provider.integer, provider.readBoolean(input));
    }

    public static Target guarded(Provider provider, Object input) {
        try {
            return new Target((long) provider.wide, (double) provider.narrow, (float) provider.number,
                (byte) provider.integer, (short) provider.integer, (char) provider.integer, provider.readBoolean(input));
        } catch (Throwable failure) {
            Trace.last = failure;
            return null;
        }
    }

    public static Target locked(Provider provider, Object input) {
        synchronized (Trace.lock) {
            return new Target((long) provider.wide, (double) provider.narrow, (float) provider.number,
                (byte) provider.integer, (short) provider.integer, (char) provider.integer, provider.readBoolean(input));
        }
    }
}
