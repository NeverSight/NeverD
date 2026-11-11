package fixture;
import java.util.Collection;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

public final class Core {
    public volatile ExecutorService service;
    public volatile Worker worker;
    public Core(ExecutorService service, Worker worker) {
        this.service = service;
        this.worker = worker;
    }
    public <T> List<Future<T>> batch(Collection<? extends Callable<T>> input) throws InterruptedException {
        return service.invokeAll(input);
    }
    public <T> List<Future<T>> scheduled(Collection<? extends Callable<T>> input, long timeout, TimeUnit unit) throws InterruptedException {
        return service.invokeAll(input, timeout, unit);
    }
    public <T> List<Future<T>> owned(Collection<? extends Callable<T>> input) throws InterruptedException {
        return worker.batch(input);
    }
    public <T> List<Future<T>> ownedScheduled(Collection<? extends Callable<T>> input, long timeout, TimeUnit unit) throws InterruptedException {
        return worker.batch(input, timeout, unit);
    }
}
