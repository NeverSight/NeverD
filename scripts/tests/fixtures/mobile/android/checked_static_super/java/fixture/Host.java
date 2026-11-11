package fixture;
public class Host extends Root {
    private static final void forward(Host receiver) {
        Guard.check(receiver, "owned receiver");
        ((Root) receiver).ping();
    }
    public static void invoke(Host receiver) { forward(receiver); }
    @Override public void ping() { Trace.event(9); }
}
