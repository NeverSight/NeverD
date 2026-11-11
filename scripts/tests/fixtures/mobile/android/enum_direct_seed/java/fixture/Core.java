package fixture;
import java.util.Collection;
import java.util.EnumSet;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Carrier direct(Enum token) {return Carrier.make(token);}
    public static Object ignored(Enum token) {Carrier.make(token);return Trace.marker;}
    public static Carrier checked(Object token) {return Carrier.make((Enum)token);}
    public static <E extends Enum<E>> Carrier<E> relay(E token) {return Carrier.make(token);}
    public static EnumSet empty(Enum token) {return EnumSet.of(token);}
    public static <T> Collection<T> unknown(T token) {return (EnumSet)EnumSet.of((Enum)token);}
}
