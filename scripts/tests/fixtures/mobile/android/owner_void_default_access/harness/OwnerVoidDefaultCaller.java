import fixture.*;
import kotlin.Unit;
import kotlin.jvm.functions.Function2;
public final class OwnerVoidDefaultCaller {
    public static void call(DefaultApi receiver, Function2<Object,Object,Unit> block, Object value) {
        DefaultApi.DefaultImpls.work(receiver, block, value);
    }
}
