import fixture.Core;
import fixture.Factory;
import java.util.function.Function;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class ConfiguredCollectorCaller {
    public static Object call(int route, CharSequence separator, CharSequence prefix,
            CharSequence suffix, Function mapper) {
        if (route == 0) return Core.returned(separator, mapper);
        if (route == 1) return Core.ignored(separator, mapper);
        if (route == 2) return Core.configured(separator, prefix, suffix, mapper);
        return Factory.make(separator, prefix, suffix, mapper);
    }
}
