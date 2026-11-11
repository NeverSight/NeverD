package fixture;
import java.util.List;
public final class Provider {
    @SuppressWarnings("unchecked") public static <T> List<Target<T>> transfer(List<Source<T>> values,T seed) {
        Trace.hit(1);
        Trace.values=values;
        Trace.seed=seed;
        Trace.action.run();
        Trace.hit(2);
        return (List<Target<T>>)Trace.result;
    }
}
