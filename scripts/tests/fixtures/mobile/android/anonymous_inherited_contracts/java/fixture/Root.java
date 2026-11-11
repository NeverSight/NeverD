package fixture;
import java.util.concurrent.Executor;
public abstract class Root<R> implements Reader<R> {
    public final Object initial;
    public int listenerCalls;
    public Runnable lastTask;
    public Executor lastExecutor;
    public Root() { initial = pending(); }
    protected abstract R pending();
    public final R read() { return pending(); }
    public final void listen(Runnable task, Executor executor) {
        ++listenerCalls;
        lastTask = task;
        lastExecutor = executor;
        executor.execute(task);
    }
}
