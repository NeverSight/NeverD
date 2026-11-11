import fixture.Client;
import java.util.Comparator;
public final class ComparablePairFactoryCaller {
    public static Comparator call(){return Client.call();}
    public static Comparator full(){return Client.full();}
}
