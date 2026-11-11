package fixture;

import java.util.function.Function;
import java.util.stream.Collector;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core {
    private Core() {}
    public static Collector returned(CharSequence separator, Function mapper) {
        return Factory.make(separator, mapper);
    }
    public static Object ignored(CharSequence separator, Function mapper) {
        Factory.make(separator, mapper);
        return Trace.result;
    }
    public static Collector configured(CharSequence separator, CharSequence prefix,
            CharSequence suffix, Function mapper) {
        return Factory.make(separator, prefix, suffix, mapper);
    }
}
