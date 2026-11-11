package fixture;
import java.util.List;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Client {
    public static List call(Comparable[] values) {
        return Api.select(values);
    }
    public static String checked(Comparable[] values) {
        return (String)Api.select(values).get(0);
    }
}
