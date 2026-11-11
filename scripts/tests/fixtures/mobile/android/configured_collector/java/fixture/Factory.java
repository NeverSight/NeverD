package fixture;

import java.util.function.Function;
import java.util.stream.Collector;

public final class Factory {
    private Factory() {}
    public static <T> Collector<T, ?, String> make(CharSequence separator,
            Function<T, ? extends CharSequence> mapper) {
        Trace.shortCalls++;
        return make(separator, Trace.prefixConfig, Trace.suffixConfig, mapper);
    }
    @SuppressWarnings("unchecked")
    public static <T> Collector<T, ?, String> make(CharSequence separator,
            CharSequence prefix, CharSequence suffix,
            Function<T, ? extends CharSequence> mapper) {
        Trace.targets++;
        Trace.separator = separator;
        Trace.prefix = prefix;
        Trace.suffix = suffix;
        Trace.mapper = mapper;
        Object input = Trace.source;
        Trace.seen = input;
        Trace.hit(1);
        Trace.mapped = mapper.apply((T)input);
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
