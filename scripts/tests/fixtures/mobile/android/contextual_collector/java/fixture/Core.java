package fixture;

import java.util.stream.Collector;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core {
    private Core() {}
    public static Collector returned(CharSequence separator) {
        return Factory.make(separator);
    }
    public static Object ignored(CharSequence separator) {
        Factory.make(separator);
        return Trace.result;
    }
    public static Collector configured(CharSequence separator, CharSequence prefix,
            CharSequence suffix) {
        return Factory.make(separator, prefix, suffix);
    }
}
