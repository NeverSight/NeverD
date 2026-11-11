package fixture;
public final class Client {
    public static Target make(int selector) {
        try {
            Trace.hit(5);
            switch (selector) {
                case 0: throw new IllegalStateException(Trace.message());
                case 1: return null;
                default: return new Target();
            }
        } catch (RuntimeException failure) {
            Trace.caught = failure;
            Trace.hit(3);
            return null;
        }
    }
}
