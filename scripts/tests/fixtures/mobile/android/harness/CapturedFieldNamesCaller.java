import fixture.*;
import java.util.function.LongSupplier;
public final class CapturedFieldNamesCaller {
    public static LongSupplier make(Core core, long wide, Object token, Hook hook) { return core.make(wide,token,hook); }
    public static long call(LongSupplier supplier) { return supplier.getAsLong(); }
}
