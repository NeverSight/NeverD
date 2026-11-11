package fixture;
public class Builder<T extends Bound> {
    public static Builder<?> latest;
    public Carrier<? extends T> source;
    public T value;
    public Object marker;
    public int id, stage;
    public Builder(Carrier<? extends T> source, int id) {
        latest = this;
        this.source = source;
        this.id = id;
        stage = 1;
        marker = source.probe.observe(this, source);
        stage = stage * 10 + 2;
        value = source.value;
        if (source.failure != null) throw source.failure;
        stage = stage * 10 + 3;
    }
}
