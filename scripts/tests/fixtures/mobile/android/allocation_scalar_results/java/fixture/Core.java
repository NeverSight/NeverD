package fixture;

public class Core {
    public static Target create(Provider provider, Object input) {
        return new Target(provider.readLong(input), provider.readDouble(input), provider.readFloat(input),
                provider.readByte(input), provider.readShort(input), provider.readChar(input), provider.readBoolean(input));
    }

    public static Target guarded(Provider provider, Object input) {
        try {
            return new Target(provider.readLong(input), provider.readDouble(input), provider.readFloat(input),
                    provider.readByte(input), provider.readShort(input), provider.readChar(input), provider.readBoolean(input));
        } catch (Throwable failure) {
            Trace.last = failure;
            return null;
        }
    }

    public static Target locked(Provider provider, Object input) {
        synchronized (Trace.lock) {
            return new Target(provider.readLong(input), provider.readDouble(input), provider.readFloat(input),
                    provider.readByte(input), provider.readShort(input), provider.readChar(input), provider.readBoolean(input));
        }
    }
}
