package fixture;

public class Closed implements Contract<Tag, Tag> {
    public int calls;
    public Tag last;
    public RuntimeException failure;

    public Closed() {}

    public Tag echo(Tag value) {
        calls++;
        last = value;
        if (failure != null) throw failure;
        return value;
    }

    public Tag echo(Tag value, int delta) {
        calls += delta;
        last = value;
        if (failure != null) throw failure;
        return value;
    }

    public Tag[] array(Tag[] values) {
        calls++;
        if (failure != null) throw failure;
        last = values[0];
        return values;
    }

    public Tag[] newArray(int size) {
        calls++;
        if (failure != null) throw failure;
        return new Tag[size];
    }
}
