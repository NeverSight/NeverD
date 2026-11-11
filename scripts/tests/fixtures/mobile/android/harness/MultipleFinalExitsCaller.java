import fixture.*;
public final class MultipleFinalExitsCaller {
    public static Core create(long a,long b) { return new Core(a,b); }
    public static long read() { return StaticCore.second; }
}
