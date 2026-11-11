import fixture.Core;
import java.util.List;
public final class RecursiveVoidCarrierCaller {
    public static void call(int entry, List values) {
        if (entry == 0) Core.create(values);
        else if (entry == 1) Core.guarded(values);
        else Core.locked(values);
    }
}
