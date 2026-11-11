package fixture;
public class Client {
    public static <T> int call(Filter<T> filter,T[] values){return Provider.index(filter,values);}
    public static <T> int from(Filter<T> filter,int start,T[] values){return Provider.from(filter,start,values);}
}
