package fixture;
import java.util.Map;
import java.lang.reflect.Type;
public final class Child extends Base<Object,Map> {
    public final Type type;
    public Child(Object source, Map target, Type type, Config config) {
        super(source,target,config);
        this.type = type;
        Trace.type = type;
        Trace.children++;
        Trace.stage = 4;
    }
}
