import fixture.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class BoundedFormalSuperCallCaller {
    public static boolean call(Base receiver, Context context, Bound value, int bits) {
        return receiver.run(context, value, bits);
    }
}
