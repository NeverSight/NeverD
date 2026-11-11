package fixture;
public class Core {
    public static View make(Fn<? super Input, Unit> callback) {
        Trace.hit(6);
        View result = new Owner(callback);
        Trace.hit(0);
        return result;
    }
}
