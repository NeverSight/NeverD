import fixture.Core;
import java.util.Map;
import java.util.Collection;
public final class SyntheticNullaryAllocationCaller {
    public static Core<String,Object> first(float factor,Map<String,Collection<Object>> value){return new Core<String,Object>(factor,value);}
    public static Core<String,Object> second(float factor,Map<String,Collection<Object>> value){return new Core<String,Object>(value,factor);}
}
