package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static List direct(Collection input,Classifier classifier) {return Factory.group(input,classifier);}
    public static ArrayList checked(Collection input,Classifier classifier) {return (ArrayList)Factory.group(input,classifier);}
    public static Object ignored(Collection input,Classifier classifier) {Factory.group(input,classifier);return Trace.marker;}
    public static <E> List<List<E>> relay(Collection<E> input,Classifier<E> classifier) {return Factory.group(input,classifier);}
    public static String nested(Collection input,Classifier classifier) {return (String)((List)Factory.group(input,classifier).get(0)).get(0);}
}
