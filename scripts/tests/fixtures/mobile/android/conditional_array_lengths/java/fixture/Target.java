package fixture;
import java.io.IOException;
public final class Target {
    static { Trace.start(); }
    public int capacity, tail;
    public Target(int capacity, int tail) throws IOException {
        Trace.target = this;
        Trace.constructors++;
        Trace.hit(3);
        this.capacity = capacity;
        this.tail = tail;
        Trace.holder.first = Trace.replacement;
        Trace.hit(4);
    }
}
