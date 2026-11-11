import fixture.Child;
import java.util.function.Function;
public final class ScopedNullSuperCaller {
    public static Child<String> defaults(boolean flag,Function<String,Comparable<?>> fn) {return new Child<String>(flag,fn);}
    public static Child<String> explicit(boolean flag,boolean secondary,Function<String,Comparable<?>> fn) {return new Child<String>(flag,secondary,fn);}
}
