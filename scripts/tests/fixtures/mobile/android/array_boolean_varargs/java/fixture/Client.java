package fixture;
public class Client {
    public static <T> boolean call(T[] values){return Provider.test(values);}
    public static <T> boolean decide(T[] values){return Provider.decide(values);}
}
