package fixture;
import java.util.Comparator;
public class Client {
    public static Comparator<String> call(){return Provider.<String>select();}
    public static Comparator<String> lookup(){return Comparator.<String>reverseOrder();}
}
