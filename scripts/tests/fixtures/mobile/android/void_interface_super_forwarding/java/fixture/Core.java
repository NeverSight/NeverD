package fixture;
public class Core {
    public static Api make() { return new Owner(); }
    public static void call(Api owner) {
        Trace.hit(6);
        owner.signal();
        Trace.hit(0);
    }
}
