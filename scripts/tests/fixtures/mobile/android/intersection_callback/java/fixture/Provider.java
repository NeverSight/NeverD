package fixture;
public final class Provider {
    static { Trace.hit(9); }
    public static <T extends Base & Observer> void observe(T node,Callback<Unit> callback) {
        Trace.hit(1);Trace.node=node;
        node.observe();Trace.hit(2);Trace.callback=callback;
        callback.run();Trace.hit(3);
    }
}
