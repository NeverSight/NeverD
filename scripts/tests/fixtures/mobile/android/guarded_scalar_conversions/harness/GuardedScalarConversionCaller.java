import fixture.Core;
public final class GuardedScalarConversionCaller {
    public static Core narrow(int seed, float incoming, int mask, Object marker) { return new Core(seed, incoming, mask, marker); }
    public static Core wide(double seed, long incoming, int mask, Object marker) { return new Core(seed, incoming, mask, marker); }
}
