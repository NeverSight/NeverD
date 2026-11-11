package fixture;
import java.util.Map;
import java.lang.reflect.Type;
public final class Child<T> extends Base<T,Map<?,?>> {
    public final Type type;
    public Child(T source, Map<?,?> target, Type type, Config config) {
        super(source,target,config);
        this.type = type;
        Trace.type = type;
        Trace.children++;
        Trace.stage = 4;
    }
}
