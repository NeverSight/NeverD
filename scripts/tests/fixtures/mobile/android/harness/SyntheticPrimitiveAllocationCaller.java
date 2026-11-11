import fixture.Core;
import java.util.Map;
import java.util.Collection;
public final class SyntheticPrimitiveAllocationCaller {
    public static Core<String,Object> first(boolean flag,float factor,Map<String,Collection<Object>> value){return new Core<String,Object>(flag,factor,value);}
    public static Core<String,Object> second(boolean flag,float factor,Map<String,Collection<Object>> value){return new Core<String,Object>(value,factor,flag);}
}
