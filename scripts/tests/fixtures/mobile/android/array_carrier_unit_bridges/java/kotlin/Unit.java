package kotlin;
// Independently authored singleton with observable first initialization.
public final class Unit {
    public static final Unit INSTANCE = new Unit();
    private Unit() { fixture.Trace.events = fixture.Trace.events * 10 + 2; }
}
