import fixture.*;
import java.util.List;
public final class CollectionPredicateCaller {
    public static boolean call(List<Object> values,Predicate<Object,Boolean> predicate) { return Provider.remove(values,predicate); }
}
