package probe;
import fixture.*;
public final class MixedInterfaceSuperCaller {
    public static int call(Api owner, Context context, Input input, int value) {
        Trace.hit(6);
        int result = owner.measure(context, input, value);
        Trace.hit(0);
        return result;
    }
    public static long call(Api owner, Context context, Input input, long value) {
        Trace.hit(6);
        long result = owner.measure(context, input, value);
        Trace.hit(0);
        return result;
    }
    public static int direct(Api owner, Context context, Input input, int value) {
        return owner.measure(context, input, value);
    }
    public static long direct(Api owner, Context context, Input input, long value) {
        return owner.measure(context, input, value);
    }
}
