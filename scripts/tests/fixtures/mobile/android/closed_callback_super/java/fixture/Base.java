package fixture;
import java.util.function.Supplier;
public class Base<T> {
    static { Trace.hit(1); }
    public Supplier<T> factory;
    public T value;
    public Base(Supplier<T> factory) {
        Trace.parent=this;
        this.factory=factory;
        Trace.context.reply=Trace.late;
        Trace.hit(5);
        value=factory.get();
    }
    public Base(Object other) {
        Trace.hit(8);
    }
}
