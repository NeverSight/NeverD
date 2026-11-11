package fixture;
import java.util.function.Supplier;
public class Core {
    public static <E,T extends Iterable<E>> T context(T value)throws IllegalArgumentException {
        Trace.hit(1);T result=Provider.context(value,"owned-message",new Object[0]);Trace.hit(4);return result;
    }
    public static <E,T extends Iterable<E>> T throwing(T value,Supplier<RuntimeException> errors) {
        Trace.hit(1);T result=Provider.throwing(value,errors);Trace.hit(4);return result;
    }
}
