import fixture.Core;
import java.util.Map;
import java.util.function.Supplier;
import java.util.function.Function;
public final class ScopedSupplierDelegationCaller {
    public static Core<String,Object> first(Supplier<Map<String,Object>> value,Function<Object,String> keys,Function<Object,Object> values){return new Core<String,Object>(value,keys,values);}
    public static Core<String,Object> second(Supplier<Map<String,Object>> value,Function<Object,String> keys,Function<Object,Object> values){return new Core<String,Object>(value,keys,values,7);}
}
