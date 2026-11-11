package fixture;

public class Core {
    public Object value;
    public long number;

    private Object read(Object input, long mask) {
        Trace.hit(1); Trace.consume(input);
        number ^= mask;
        return value;
    }

    private long readLong(Object input, long mask) {
        Trace.hit(2); Trace.consume(input);
        value = null;
        return number ^ mask;
    }

    public static Target create(Core receiver, Object input, long mask) {
        return new Target(receiver.read(input, mask), receiver.readLong(input, mask));
    }

    public static Target guarded(Core receiver, Object input, long mask) {
        try {
            return new Target(receiver.read(input, mask), receiver.readLong(input, mask));
        } catch (Throwable failure) {
            Trace.last = failure;
            return null;
        }
    }

    public static Target locked(Core receiver, Object input, long mask) {
        synchronized (Trace.lock) {
            return new Target(receiver.read(input, mask), receiver.readLong(input, mask));
        }
    }
}
