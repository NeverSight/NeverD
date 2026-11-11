import fixture.*;
public final class RecursiveArrayCarrierResultCaller {
    public static Object call(int operation, Comparable[] values) {
        return operation == 0 ? Client.call(values) : Client.checked(values);
    }
}
