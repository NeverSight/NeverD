import fixture.Client;
import java.util.Comparator;
public final class RecursiveCarrierFactoryCaller {
    public static Comparator<String> call(){return Client.call();}
    public static Comparator<String> lookup(){return Client.lookup();}
}
