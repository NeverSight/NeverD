package fixture;

import java.util.List;

public final class GenericBox<T> {
    public T value;
    public T[] values;
    public List<? extends T> upper;
    public List<? super T> lower;
    public List<?> any;

    public GenericBox(T initial) {
        value = initial;
    }

    public T get() {
        return value;
    }

    public T exchange(T next) {
        T previous = value;
        value = next;
        return previous;
    }

    public T[] arrayIdentity(T[] input) {
        return input;
    }

    public T first(T[] input, int index) {
        return input[index];
    }

    public T arrayExchange(T[] input, int index, T next) {
        T previous = input[index];
        input[index] = next;
        return previous;
    }

    public <T extends Number> T shadow(T input) {
        return input;
    }
}
