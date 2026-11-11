package fixture;

public final class Caller {
    private Caller() {}

    public static boolean primitive(Receiver<?, ?> receiver) {
        return receiver.primitiveInvoke();
    }

    public static Object direct(Receiver<?, ?> receiver) {
        return receiver.invoke();
    }

    public static Object erased(Function<?> receiver) {
        return receiver.invoke();
    }
}
