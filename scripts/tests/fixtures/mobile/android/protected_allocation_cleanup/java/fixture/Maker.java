package fixture;

public class Maker {
    public static Object create(Object input, boolean clean) {
        try {
            try {
                return new Problem("owned protected cleanup");
            } catch (Throwable failure) {
                Trace.record(failure, input);
                return failure;
            }
        } finally {
            if (clean) Trace.cleanup(1);
            Trace.cleanup(2);
        }
    }
}
