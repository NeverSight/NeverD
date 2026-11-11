package fixture;
import java.util.Collection;
import java.util.concurrent.*;
public final class Core {
    public volatile ExecutorService service;
    public volatile Worker worker;
    public Core(ExecutorService service, Worker worker) {
        this.service = service;
        this.worker = worker;
    }
    public <T> T single(Collection<? extends Callable<T>> input) throws InterruptedException, ExecutionException {
        return service.invokeAny(input);
    }
    public <T> T scheduled(Collection<? extends Callable<T>> input, long timeout, TimeUnit unit) throws InterruptedException, ExecutionException, TimeoutException {
        return service.invokeAny(input, timeout, unit);
    }
    public <T> T owned(Collection<? extends Callable<T>> input) throws InterruptedException, ExecutionException {
        return worker.choose(input);
    }
    public <T> T ownedScheduled(Collection<? extends Callable<T>> input, long timeout, TimeUnit unit) throws InterruptedException, ExecutionException, TimeoutException {
        return worker.choose(input, timeout, unit);
    }
}
