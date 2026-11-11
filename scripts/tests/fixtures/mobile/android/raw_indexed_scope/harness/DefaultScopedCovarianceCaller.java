import kotlin.jvm.functions.Function2;
public class DefaultScopedCovarianceCaller {
    public static Object call(Function2<Object,Object,Object> callback, Object index, Object value) { return callback.invoke(index, value); }
}
