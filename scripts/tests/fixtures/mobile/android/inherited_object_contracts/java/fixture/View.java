package fixture;

public class View<T> implements Contract<T> {
    public int equalsOverloads;
    public int copies;
    public int arrays;
    public int values;
    public Object lastTag;
    public RuntimeException failure;

    public View() {}

    public <R> R[] toArray(R[] input) {
        arrays++;
        if (failure != null) throw failure;
        return input;
    }

    public <T, U> T[] copy(T[] input, U tag) {
        copies++;
        lastTag = tag;
        if (failure != null) throw failure;
        return input;
    }

    public T value(T next) {
        values++;
        if (failure != null) throw failure;
        return next;
    }
    public boolean equals(String other) {
        equalsOverloads++;
        if (failure != null) throw failure;
        return false;
    }

}
