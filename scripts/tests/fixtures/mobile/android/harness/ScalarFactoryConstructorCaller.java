import fixture.*;
import java.util.function.Function;
public final class ScalarFactoryConstructorCaller {
    public static Target<String,String> create(int entry,int capacity,boolean flag,Function<String,String> mapper) {
        if(entry==0)return new Target<String,String>(flag,mapper);
        if(entry==1)return new Target<String,String>(capacity,flag,mapper);
        return new Target<String,String>(mapper);
    }
    public static String value(Target<String,String> target) { return target.box.map.get(Trace.key); }
    public static String apply(Target<String,String> target,String value) { return target.mapper.apply(value); }
}
