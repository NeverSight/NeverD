package fixture;
public class Completion<T> {
    public T value;
    public RuntimeException failure;
    public boolean done;
    public void complete(T value) {
        this.value = value;
        done = true;
        Trace.hit(7);
    }
}
