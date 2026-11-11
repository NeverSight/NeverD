import fixture.Core;
import java.util.Comparator;
public final class ScopedComparatorAllocationCaller {
    public static Core<String,Object> first(Comparator<Object> value){return new Core<String,Object>(value);}
    public static Core<String,Object> second(Comparator<Object> value){return new Core<String,Object>(value,7);}
}
