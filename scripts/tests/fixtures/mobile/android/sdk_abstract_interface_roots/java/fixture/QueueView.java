package fixture;
import java.util.PriorityQueue;
public final class QueueView<E, F> extends PriorityQueue<E> {
    public int calls;
    public QueueView() { super(); }
    @Override
    public boolean offer(E value) { calls++; return super.offer(value); }
}
