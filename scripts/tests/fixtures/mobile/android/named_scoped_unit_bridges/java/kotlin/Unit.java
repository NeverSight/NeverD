package kotlin;
public final class Unit {
    public static final Unit INSTANCE = initialize();
    private Unit() {}
    private static Unit initialize() {
        fixture.Trace.initializations++;
        if (fixture.Trace.initializationFailure != null) throw fixture.Trace.initializationFailure;
        return new Unit();
    }
}
