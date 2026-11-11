package fixture;
import java.util.Collection;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
public final class NestedTaskTransferCaller {
    public static <T> List<Future<T>> call(Core core, int operation, Collection<? extends Callable<T>> input, long timeout, TimeUnit unit) throws InterruptedException {
        if (operation == 0) return core.batch(input);
        if (operation == 1) return core.scheduled(input, timeout, unit);
        if (operation == 2) return core.owned(input);
        return core.ownedScheduled(input, timeout, unit);
    }
}
