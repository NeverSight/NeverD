package fixture;
import java.util.Collection;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
public interface Worker {
    <Element> List<Future<Element>> batch(Collection<? extends Callable<Element>> input) throws InterruptedException;
    <Element> List<Future<Element>> batch(Collection<? extends Callable<Element>> input, long timeout, TimeUnit unit) throws InterruptedException;
}
