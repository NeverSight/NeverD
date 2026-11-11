package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <E> int[] locate(Collection<E> source,Test<E> test) {
        Trace.calls++;Trace.source=source;Trace.test=test;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (int[])Trace.result;
    }
    public static <E> int[] from(List<E> source,Test<E> test) {
        return locate(source,test);
    }
}
