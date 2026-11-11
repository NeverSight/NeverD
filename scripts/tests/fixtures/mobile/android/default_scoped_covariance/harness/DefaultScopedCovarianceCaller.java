import kotlin.jvm.functions.Function1;
public class DefaultScopedCovarianceCaller {
    public static Object call(Function1<Object,Object> callback, Object value) { return callback.invoke(value); }
}
