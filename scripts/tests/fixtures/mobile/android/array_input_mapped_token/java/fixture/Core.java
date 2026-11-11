package fixture;
import java.util.function.Function;
public final class Core {
    private Core() {}
    public static <Source,Result> Result[] call(Source[] source, Class<Result> type, Function<? super Source,? extends Result> mapper) {
        return Provider.map(source,type,mapper);
    }
    public static Object[] checked(Object source, Object type, Object mapper) {
        return call((Object[])source,(Class)type,(Function)mapper);
    }
    public static String[] narrow(Object[] source, Class<String> type, Function<Object,String> mapper) {
        return call(source,type,mapper);
    }
}
