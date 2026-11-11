package fixture;

@SuppressWarnings("rawtypes")
public class Child extends Base implements Producer<Value> {
    public Value value;
    public RuntimeException failure;
    public int reads;
    public int echoes;
    public Child(Value value) {
        super(0);
        this.value = value;
    }
    public Value read() {
        ++reads;
        if (failure != null) throw failure;
        return value;
    }
    public Value echo(Value value) {
        ++echoes;
        if (failure != null) throw failure;
        return value;
    }
}
