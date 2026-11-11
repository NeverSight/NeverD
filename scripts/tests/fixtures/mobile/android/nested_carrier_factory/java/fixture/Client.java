package fixture;
import java.util.*;
public class Client {
    public static Factory<String> call(List<Comparator<String>> source) { return Factory.from(source); }
    public static Comparator<String> read(List<Comparator<String>> source) { return Factory.from(source).values.get(0); }
}
