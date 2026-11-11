package fixture;

public final class Result {
    static { AllocationState.initialize(); }
    public static int constructions;
    public static RuntimeException failure;
    public final Object token;
    public final long stamp;
    private static void checkFailure() {
        ++constructions;
        if (failure != null) throw failure;
    }
    public Result(Object token, long stamp) {
        checkFailure();
        this.token = token;
        this.stamp = stamp;
    }
}
