package fixture;
import java.util.Collection;
import java.util.concurrent.*;
public interface Worker {
    <Element> Element choose(Collection<? extends Callable<Element>> input) throws InterruptedException, ExecutionException;
    <Element> Element choose(Collection<? extends Callable<Element>> input, long timeout, TimeUnit unit) throws InterruptedException, ExecutionException, TimeoutException;
}
