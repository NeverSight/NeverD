package kotlin;

// Independently authored nominal API with observable initialization. It is
// never taken from a dependency or used as a substitute for real APK code.
public final class Unit {
    public static final Unit INSTANCE = new Unit();
    private Unit() { fixture.Trace.events = fixture.Trace.events * 10 + 2; }
}
