import fixture.Core;
import java.lang.ref.Reference;
import java.util.concurrent.ConcurrentMap;
public final class ScopedStaticFieldSuperCaller {
    public static Core<String,String> first(ConcurrentMap<Reference<String>,String> value){return new Core<String,String>(value);}
    public static Core<String,String> second(ConcurrentMap<Reference<String>,String> value){return new Core<String,String>(value,7);}
}
