package fixture;
import java.util.function.Supplier;
public abstract class Base<T> {
    static { Trace.hit(1); }
    public Supplier<? extends T> factory;
    public T value;
    public Base(Supplier<? extends T> factory) {
        Trace.parent = this;
        this.factory = factory;
        Trace.seed = Trace.late;
        Trace.hit(5);
        value = factory.get();
    }
}
