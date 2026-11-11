package fixture;
import java.util.Map;
import java.lang.reflect.Type;
public final class Child<T> extends Base<T,Map<? extends String,? super String>> {
    public final Type type;
    public Child(T source, Map<? extends String,? super String> target, Type type, Config config) {
        super(source,target,config);
        this.type = type;
        Trace.type = type;
        Trace.children++;
        Trace.stage = 4;
    }
}
