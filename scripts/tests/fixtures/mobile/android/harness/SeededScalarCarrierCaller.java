import fixture.Core;
import java.util.List;
public final class SeededScalarCarrierCaller {
    public static int run(int entry, List values, Object needle) {
        if (entry == 0) return Core.create(values, needle);
        if (entry == 1) return Core.guarded(values, needle);
        return Core.locked(values, needle);
    }
}
