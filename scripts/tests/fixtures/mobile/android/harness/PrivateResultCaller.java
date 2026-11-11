import fixture.*;
public final class PrivateResultCaller {
    public static Object raw(Owner.Worker caller, Owner.Worker receiver, String label, Class<?>[] kinds, Object[] values, long wide, double real) {
        return caller.raw(receiver, label, kinds, values, wide, real);
    }
    public static String narrow(Owner.Worker caller, Owner.Worker receiver, String label, Class<?>[] kinds, Object[] values, long wide, double real) {
        return caller.narrow(receiver, label, kinds, values, wide, real);
    }
}
