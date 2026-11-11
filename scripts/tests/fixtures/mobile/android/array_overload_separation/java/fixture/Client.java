package fixture;
import java.io.File;
import java.util.Collection;
public class Client {
    public static Child call(String[] data,Collection<File> values){return new Child(data,values);}
    public static Child fresh(Collection<File> values){return new Child(values);}
}
