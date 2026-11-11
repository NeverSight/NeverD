package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Object direct(Iterator cursor,int offset) {return Factory.at(cursor,offset);}
    public static Object from(Iterable source,int offset) {return Factory.from(source,offset);}
    public static String checked(Iterator cursor,int offset) {return (String)Factory.at(cursor,offset);}
    public static Object ignored(Iterator cursor,int offset) {Factory.at(cursor,offset);return Trace.marker;}
    public static <E> E relay(Iterator<E> cursor,int offset) {return Factory.at(cursor,offset);}
}
