package fixture;

import java.io.IOException;
import java.util.List;

public class Client {
    public static <T extends Number> List<T> call(List<T> values, int first, int second) throws IOException {
        return call(values,first,second,false);
    }
    public static <T extends Number> List<T> call(List<T> values, int first, int second, boolean flag) throws IOException {
        return Provider.read(values,first,second,flag);
    }
}
