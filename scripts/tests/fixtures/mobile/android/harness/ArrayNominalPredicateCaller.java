import fixture.Api;
import fixture.Receipt;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ArrayNominalPredicateCaller {
    public static boolean call(Receipt value, Comparable argument) { return ((Api)value).contains(argument); }
}
