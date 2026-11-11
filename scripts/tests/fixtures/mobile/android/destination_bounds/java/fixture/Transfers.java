package fixture;

import java.io.IOException;
import java.util.Collection;
import java.util.function.Function;
import java.util.function.Supplier;

class Transfers {
    public static <T extends Comparable<? super T>> T first(T[] values) {
        return values[0];
    }

    public static Double first(Double[] values) {
        throw new AssertionError("wrong reference overload");
    }

    public static <T extends Comparable<? super T>> void rearrange(
            T[] values, int left, int right) throws IOException {
        T saved = values[left];
        values[left] = values[right];
        if (saved == null) throw new IOException("empty slot");
        values[right] = saved;
    }

    public static <T> void rearrange(T[] values, int left, int right) {
        throw new AssertionError("wrong generic overload");
    }

    public static void rearrange(int[] values, int left, int right) {
        throw new AssertionError("wrong overload");
    }

    public static <T> Supplier<? extends T> pick(Supplier<? extends T> source,
            Function<? super T, ? extends T> map) {
        map.apply(source.get());
        return source;
    }

    public static <T> Supplier<? extends T> pick(T seed,
            Function<? super T, ? extends T> map) {
        throw new AssertionError("wrong seed overload");
    }

    public static <E> int choose(Collection<? extends E> from, E[] tail) {
        return from.size() + tail.length;
    }

    public static <E> int choose(Iterable<? extends E> from, E[] tail) {
        throw new AssertionError("wrong collection overload");
    }

    public static <E> int choose(Collection<? extends E> from, E tail) {
        throw new AssertionError("wrong element overload");
    }

    public static <E> int choose(Iterable<? extends E> from, E tail) {
        throw new AssertionError("wrong iterable overload");
    }

    public static <D extends Collection<? super E>, E> D select(
            Object[] values, D destination, Class<E> type) throws IOException {
        for (Object value : values) {
            if (type.isInstance(value)) destination.add(type.cast(value));
        }
        return destination;
    }

    public static <E, D extends Collection<? super E>> D append(
            E[] values, D destination) {
        for (E value : values) destination.add(value);
        return destination;
    }

    public static <D extends Collection<? super E>, E> D append(
            Iterable<? extends E> values, D destination) {
        for (E value : values) destination.add(value);
        return destination;
    }

    public static <D extends Collection<? super Integer>> D append(
            int[] values, D destination) {
        for (int value : values) destination.add(value);
        return destination;
    }
}
