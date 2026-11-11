package fixture;
import java.util.function.Function;
public final class ArrayInputMappedTokenCaller {
    public static <Source,Result> Result[] call(Source[] source, Class<Result> type, Function<? super Source,? extends Result> mapper) { return Core.call(source,type,mapper); }
    public static Object[] checked(Object source, Object type, Object mapper) { return Core.checked(source,type,mapper); }
    public static String[] narrow(Object[] source, Class<String> type, Function<Object,String> mapper) { return Core.narrow(source,type,mapper); }
}
