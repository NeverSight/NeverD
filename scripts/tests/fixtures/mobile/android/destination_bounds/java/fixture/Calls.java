package fixture;

import java.io.IOException;
import java.util.Arrays;
import java.util.Collection;
import java.util.function.Function;
import java.util.function.Supplier;

final class TransferView extends Transfers { }

public final class Calls {
    public static <T extends Comparable<? super T>> T head(T[] values) {
        return TransferView.first(values);
    }

    public static void exchange(String[] values, int left, int right) throws IOException {
        TransferView.rearrange(values, left, right);
    }

    public static Supplier<? extends String> pick(Supplier<String> source,
            Function<String, String> map) {
        return TransferView.<String>pick(source, map);
    }

    public static int choose(Collection<String> from, String[] tail) {
        return TransferView.choose(from, tail);
    }

    public static Collection<Object> select(Object[] values, Collection<Object> destination)
            throws IOException {
        return Transfers.select(values, destination, String.class);
    }

    public static Collection<Object> append(String[] values, Collection<Object> destination) {
        return Transfers.append(values, destination);
    }

    public static Collection<Object> append(Iterable<String> values, Collection<Object> destination) {
        return Transfers.append(values, destination);
    }

    public static Collection<Object> append(int[] values, Collection<Object> destination) {
        return Transfers.append(values, destination);
    }

    public static CharSequence[] range(Object[] values, int start, int end) {
        return Arrays.copyOfRange(values, start, end, CharSequence[].class);
    }
}
