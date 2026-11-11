import fixture.*;
public final class BoundedStaticSelfResultCaller {
    public static Object call(int operation, Bound seed) {
        return operation == 0 ? Client.call(seed) : Client.checked(seed);
    }
}
