package fixture;
public final class Cleanup {
    public static java.util.function.IntConsumer observer;
    private Cleanup() {}
    public static void hit(int event) {
        java.util.function.IntConsumer action = observer;
        if (action != null) action.accept(event);
    }
}
