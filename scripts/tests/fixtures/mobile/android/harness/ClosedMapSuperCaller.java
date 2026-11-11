import fixture.Core;
import java.util.Map;
public final class ClosedMapSuperCaller {
    public static Core first(Map<String,Object> value){return new Core(value);}
    public static Core second(Map<String,Object> value){return new Core(value,7);}
}
