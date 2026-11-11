package fixture;
public class Maker {
    public static Object create(Object input) {
        try {
            return new Problem("owned create");
        } catch (IllegalArgumentException failure) {
            Trace.logFailure(failure, input); return failure;
        } catch (Throwable failure) {
            Trace.logFailure(failure, input); return failure;
        }
    }
    public static Object raised(Object input) {
        try {
            throw new Problem("owned raised");
        } catch (Problem failure) {
            Trace.logFailure(failure, input); return failure;
        } catch (Throwable failure) {
            Trace.logFailure(failure, input); return failure;
        }
    }
}
