package probe;
import fixture.*;
public final class VoidInterfaceSuperCaller {
    public static void call(Api owner) {
        Trace.hit(6);
        owner.signal();
        Trace.hit(0);
    }
    public static void direct(Api owner) { owner.signal(); }
}
