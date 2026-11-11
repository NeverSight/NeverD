package fixture;
import java.util.*;
import java.util.function.*;
public class Client {
    public static Comparator call(Function fn,boolean flag,Object[] keys) { return Provider.create(fn,flag,keys); }
    public static Comparator defaults(Function fn,Object[] keys) { return Provider.create(fn,keys); }
}
