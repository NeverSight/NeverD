import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class FinalSuperclassParameterBridgeCaller {
    public static class Child extends Core {}

    @SuppressWarnings("unchecked")
    public static void call(int route, Core receiver, Wide value, Object note) throws Throwable {
        Trace.hit(8);
        if (note instanceof int[]) ((int[]) note)[0] += 2;
        long stamp = 0x87654321abcdef09L ^ Trace.seed;
        if (route == 0) receiver.accept((Narrow) value, stamp, note);
        else if (route == 1) ((Root) receiver).accept(value, stamp, note);
        else if (route == 2) {
            try {
                Core.class.getDeclaredMethod("accept", Wide.class, long.class, Object.class)
                    .invoke(receiver, value, stamp, note);
            } catch (InvocationTargetException failure) { throw failure.getCause(); }
        } else {
            MethodHandles.lookup().findVirtual(Core.class, "accept",
                MethodType.methodType(void.class, Wide.class, long.class, Object.class))
                .invokeExact(receiver, value, stamp, note);
        }
    }
}
