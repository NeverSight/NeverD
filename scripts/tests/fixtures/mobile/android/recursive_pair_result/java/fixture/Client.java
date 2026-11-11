package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Client {
    public static Comparable call(Comparable left, Comparable right) {
        return Api.select(left, right);
    }
    public static String checked(Comparable left, Comparable right) {
        return (String)Api.select(left, right);
    }
}
