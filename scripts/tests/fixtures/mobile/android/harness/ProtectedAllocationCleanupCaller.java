import fixture.Maker;

public final class ProtectedAllocationCleanupCaller {
    public static Object call(Object input, boolean clean) {
        return Maker.create(input, clean);
    }
}
