import fixture.*;
import java.util.*;
public final class NestedCarrierFactoryCaller {
    public static Factory<String> call(List<Comparator<String>> source) { return Client.call(source); }
    public static Comparator<String> read(List<Comparator<String>> source) { return Client.read(source); }
}
