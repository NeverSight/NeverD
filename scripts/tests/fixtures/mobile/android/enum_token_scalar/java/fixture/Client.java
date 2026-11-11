package fixture;
public class Client {public static <E extends Enum<E>> E call(Class<E> type,int index){return Provider.select(type,index);}}
