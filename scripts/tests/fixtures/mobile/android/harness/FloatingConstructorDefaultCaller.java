import fixture.Core;
import java.util.Map;
public final class FloatingConstructorDefaultCaller {
    public static Core<String,Object> first(Map<String,Object> value){return new Core<String,Object>(value);}
    public static Core<String,Object> second(Map<String,Object> value){return new Core<String,Object>(value,7);}
}
