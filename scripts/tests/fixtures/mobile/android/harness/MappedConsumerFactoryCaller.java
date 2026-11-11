import fixture.Core;
import fixture.Rule;
import java.util.Comparator;
import java.util.function.Function;

public class MappedConsumerFactoryCaller {
    public static Rule owned(Core core) { return core.run(); }
    public static Comparator jdk(Core core, Function mapper, Comparator comparator) {
        return core.jdk(mapper, comparator);
    }
}
