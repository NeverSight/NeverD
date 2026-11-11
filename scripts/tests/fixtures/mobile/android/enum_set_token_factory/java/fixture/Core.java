package fixture;
import java.util.Collection;
import java.util.EnumSet;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Carrier direct(Class token) {return Carrier.make(token);}
    public static Object ignored(Class token) {Carrier.make(token);return Trace.marker;}
    public static Carrier checked(Object token) {return Carrier.make((Class)token);}
    public static <E extends Enum<E>> Carrier<E> relay(Class<E> token) {return Carrier.make(token);}
    public static EnumSet empty(Class token) {return EnumSet.noneOf(token);}
    public static <T> Collection<T> unknown(Class<T> token) {return (EnumSet)EnumSet.noneOf((Class)token);}
}
