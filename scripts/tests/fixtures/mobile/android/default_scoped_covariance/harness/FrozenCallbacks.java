import fixture.*;
import kotlin.jvm.functions.Function1;
public class FrozenCallbacks {
    public static Function1<Object,Object> originalDefault() {
        Callbacks.make$default(null, 1, null);
        return (Function1<Object,Object>) Trace.callback;
    }
}
