import fixture.*;
public final class RecursivePairResultCaller {
    public static Object call(int operation, Comparable left, Comparable right) {
        return operation == 0 ? Client.call(left, right) : Client.checked(left, right);
    }
}
