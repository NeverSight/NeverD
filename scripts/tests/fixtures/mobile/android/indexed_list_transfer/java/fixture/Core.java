package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static List direct(List input,int index) {return Factory.make(input,index);}
    public static ArrayList checked(ArrayList input,int index) {return (ArrayList)Factory.make(input,index);}
    public static Object ignored(List input,int index) {Factory.make(input,index);return Trace.marker;}
    public static <E> List<List<E>> relay(List<E> input,int index) {return Factory.make(input,index);}
    @Deprecated public static <E> List<List<E>> old(List<E> input,int index) {return Factory.make(input,index);}
}
