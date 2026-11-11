package fixture;
import java.util.concurrent.Executor;
public interface Reader<R> {
    R read();
    void listen(Runnable task, Executor executor);
    default boolean ready() { return true; }
}
