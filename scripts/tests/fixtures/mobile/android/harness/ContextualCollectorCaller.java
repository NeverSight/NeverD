import fixture.Core;
import fixture.Factory;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class ContextualCollectorCaller {
    public static Object call(int route, CharSequence separator, CharSequence prefix,
            CharSequence suffix) {
        if (route == 0) return Core.returned(separator);
        if (route == 1) return Core.ignored(separator);
        if (route == 2) return Core.configured(separator, prefix, suffix);
        return Factory.make(separator, prefix, suffix);
    }
}
