import fixture.Core;
import java.util.List;
public final class PackageArrayFactoryCaller {
    public static List<?> make(Object[] values) { return Core.make(values); }
    public static String narrowed(Object[] values) { return Core.narrowed(values); }
    public static void ignored(Object[] values) { Core.ignored(values); }
}
