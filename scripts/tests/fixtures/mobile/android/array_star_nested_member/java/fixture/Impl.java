package fixture;
import java.io.IOException;
import java.util.Set;
public class Impl implements Factory {
    public <T> Set<Box<T>> query(T value, Class<?>... types) throws IOException {
        Trace.receiver = this;
        Trace.value = value;
        Trace.types = types;
        Trace.calls++;
        Trace.hit(1);
        Trace.action.run();
        Trace.hit(2);
        return Trace.result;
    }
    public <T> Set<Box<T>> named(T value, String context, Class<?>... types) throws IOException {
        Trace.context = context;
        return query(value, types);
    }

}
