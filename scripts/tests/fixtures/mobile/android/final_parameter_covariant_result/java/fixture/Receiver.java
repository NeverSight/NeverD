package fixture;
import java.util.List;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public List<Value> first, second;
    public Object invoke(Object first, Object second) {
        return invoke((List) first, (List) second);
    }
    public final List<Value> invoke(List<Value> first, List<Value> second) {
        Trace.events++;
        this.first = first;
        this.second = second;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return first == null ? second : first;
    }
}
