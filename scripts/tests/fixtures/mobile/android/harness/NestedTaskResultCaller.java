package fixture;
import java.util.Collection;
import java.util.concurrent.*;
public final class NestedTaskResultCaller {
    public static <T> T call(Core core, int operation, Collection<? extends Callable<T>> input, long timeout, TimeUnit unit) throws InterruptedException, ExecutionException, TimeoutException {
        if (operation == 0) return core.single(input);
        if (operation == 1) return core.scheduled(input, timeout, unit);
        if (operation == 2) return core.owned(input);
        return core.ownedScheduled(input, timeout, unit);
    }
}
