import fixture.Core;
public final class StaticFinalResourceCaller {
    private StaticFinalResourceCaller() {}
    public static Object read() { return Core.value(); }
}
