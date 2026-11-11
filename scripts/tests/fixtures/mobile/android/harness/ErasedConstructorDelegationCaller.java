import fixture.*;
import kotlin.jvm.functions.Function1;
public final class ErasedConstructorDelegationCaller {
    public static Core make(int a, int b, long bits, Function1<Object,Boolean> callback) {
        return new Core.Factory().make(a, b, bits, callback);
    }
}
