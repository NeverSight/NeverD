import fixture.Core;
import java.util.SortedMap;
public final class ScopedMapInitializerUpcastCaller {
    public static Core<String,Object> first(SortedMap<String,Object> value){return new Core<String,Object>(value);}
    public static Core<String,Object> second(SortedMap<String,Object> value){return new Core<String,Object>(value,7);}
}
