import fixture.*;
import java.util.*;
import java.util.function.*;
public final class CallbackArrayFactoryCaller {
    public static Comparator call(Function fn,boolean flag,Object[] keys) {return Client.call(fn,flag,keys);}
    public static Comparator defaults(Function fn,Object[] keys) {return Client.defaults(fn,keys);}
}
