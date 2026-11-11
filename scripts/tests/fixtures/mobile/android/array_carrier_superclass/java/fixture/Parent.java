package fixture;
public abstract class Parent<C,R> {
    public Parent() {}
    protected abstract void write(Token token,C value,int index,long wide);
    protected abstract int count(C value);
    protected abstract R read(C value);
    public void send(Token token,C value,int index,long wide) { write(token,value,index,wide); }
    public int measure(C value) { return count(value); }
    public R receive(C value) { return read(value); }
}
