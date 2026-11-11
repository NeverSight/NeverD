import fixture.*;
import java.util.function.Function;
public final class PairConstructorTransferCaller {
    public static Target<String,String> create(int capacity,float load,Function<String,String> mapper) { return new Target<String,String>(capacity,load,mapper); }
    public static String value(Target<String,String> target) { return target.box.map.get(Trace.key); }
    public static String apply(Target<String,String> target,String value) { return target.mapper.apply(value); }
}
