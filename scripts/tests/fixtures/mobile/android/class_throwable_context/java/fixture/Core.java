package fixture;
import java.util.function.Function;
public final class Core {
    private Core() {}
    public static <V> V call(Box<V> box,Function<String,RuntimeException> factory,String message){return box.get(factory,message);}
    public static <V> V plain(Box<V> box){return box.plain();}
    public static String checked(Box<String> box,Function<String,RuntimeException> factory,String message,boolean plain){return plain?plain(box):call(box,factory,message);}
}
