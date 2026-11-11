package fixture;
import java.util.Map;
import java.lang.reflect.Type;
public final class Core {
    private Core() {}
    public static Child make(Object source, Map target, Type type, Config config) { return new Child(source,target,type,config); }
    public static Map target(Object source, Map target, Type type, Config config) { return (Map)new Child(source,target,type,config).target; }
    public static Object source(Object source, Map target, Type type, Config config) { return new Child(source,target,type,config).source; }
    public static Type type(Object source, Map target, Type type, Config config) { return new Child(source,target,type,config).type; }
    public static Object ignored(Object source, Map target, Type type, Config config) { new Child(source,target,type,config); return Trace.marker; }
}
