package fixture.api;
import fixture.Trace;
import java.util.Map;
import java.util.List;
import fixture.Config;
public class Provider {
    static { Trace.hit(1); }
    @SuppressWarnings("unchecked")
    public static <K> Map<K, String> make(List<K> source, Config context) {
        Trace.hit(2);
        Trace.seenSource=source;Trace.seenContext=context;Trace.snapshot=context.mark;
        ((int[]) Trace.input)[0]++;
        Trace.hit(3);
        return (Map<K, String>) Trace.result;
    }
}
