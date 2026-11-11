package fixture;

import java.util.Collection;

public final class Provider {
    private Provider() {}
    public static <T> boolean transfer(Collection<? super T> carrier, T... values) {
        Trace.hit(0, carrier, values, null);
        for (int index=0; index<values.length; ++index) {
            T value=values[index];
            Trace.hit(1, carrier, values, value);
            carrier.add(value);
        }
        Trace.hit(2, carrier, values, null);
        return Trace.returned;
    }
}
