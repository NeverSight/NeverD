import fixture.*;
import java.util.function.Function;
public final class ClassThrowableContextCaller {
    public static <Z> Z generic(Box<Z> box,Function<String,RuntimeException> factory,String message){return Core.plain(box);}
    public static String typed(Box<String> box,Function<String,RuntimeException> factory,String message){return Core.plain(box);}
}
