package kotlin;
public final class Unit {
    public static final Unit INSTANCE = new Unit();
    private Unit() { fixture.Trace.hit(3); }
}
