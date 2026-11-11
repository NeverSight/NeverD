import fixture.*;
import kotlin.jvm.functions.Function2;
public class FrozenCallbacks {
    public static Function2<Object,Object,Object> originalDefault() {
        Callbacks.make$default(null, 1, null);
        return (Function2<Object,Object,Object>) Trace.callback;
    }
}
