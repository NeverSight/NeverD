package fixture;
public class Client {
    public static <E extends Enum<E>> E defaults(Class<E> type,Object value) {return Provider.convert(type,value);}
    public static <E extends Enum<E>> E explicit(Class<E> type,Object value,E fallback) {return Provider.convert(type,value,fallback);}
}
