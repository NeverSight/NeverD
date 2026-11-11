import fixture.*;
import java.util.Map;
import java.util.Collection;
public final class ScopedIntegerGetterDelegationCaller {
    public static Core<String,Object> first(float factor,Map<String,Collection<Object>> value,Maker<Collection<Object>> maker){return new Core<String,Object>(factor,value,maker);}
    public static Core<String,Object> second(float factor,Map<String,Collection<Object>> value,Maker<Collection<Object>> maker){return new Core<String,Object>(value,factor,maker);}
}
