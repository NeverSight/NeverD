package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Object direct(Collection source,Test test) {return Factory.locate(source,test);}
    public static Object from(List source,Test test) {return Factory.from(source,test);}
    public static int element(Collection source,Test test) {return Factory.locate(source,test)[Trace.index];}
    public static Object ignored(Collection source,Test test) {Factory.locate(source,test);return Trace.marker;}
    public static <E> int[] relay(Collection<E> source,Test<E> test) {return Factory.locate(source,test);}
    public static Object write(Collection source,Test test) {int[] values=Factory.locate(source,test);values[Trace.index]=Trace.value;return values;}
}
