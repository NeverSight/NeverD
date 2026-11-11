package fixture;

import java.util.stream.Collector;

public final class Factory {
    private Factory() {}
    public static <T> Collector<T, ?, String> make(CharSequence separator) {
        Trace.shortCalls++;
        return make(separator, Trace.prefixConfig, Trace.suffixConfig);
    }
    @SuppressWarnings("unchecked")
    public static <T> Collector<T, ?, String> make(CharSequence separator,
            CharSequence prefix, CharSequence suffix) {
        Trace.targets++;
        Trace.separator = separator;
        Trace.prefix = prefix;
        Trace.suffix = suffix;
        Trace.hit(1);
        Trace.before.run();
        Trace.hit(2);
        Trace.separatorLength = separator.length();
        Trace.hit(3);
        Trace.prefixLength = prefix.length();
        Trace.hit(4);
        Trace.suffixLength = suffix.length();
        Trace.hit(5);
        Trace.action.run();
        Trace.hit(6);
        return (Collector<T, ?, String>)Trace.result;
    }
}
