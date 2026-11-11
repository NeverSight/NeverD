package fixture;
public class Client {
    public static <E extends Enum<E>> E call(Class<E> type,String text){return Provider.select(type,text);}
    public static <E extends Enum<E>> E lookup(Class<E> type,String text){return Enum.valueOf(type,text);}
}
