package fixture;
public final class Receiver<R> implements Source<R> {
    public int calls;
    public Receiver() {}
    @Override
    public void touch() { calls++; }
}
