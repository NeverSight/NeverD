package fixture;
public final class ObjectPrefixDefaultsCaller {
    public static View make(Fn<? super Input, Unit> callback) {
        Trace.hit(6);
        View result = new Owner(callback);
        Trace.hit(0);
        return result;
    }
    public static View direct(Fn<? super Input, Unit> callback) { return new Owner(callback); }
}
