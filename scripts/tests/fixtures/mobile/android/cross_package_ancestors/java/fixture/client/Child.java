package fixture.client;

import fixture.base.Parent;

public class Child<S extends Tag, U extends Tag> extends Parent<S> {
    public S last;
    public int calls;
    public int ownCalls;
    public Child(int seed) { super(seed); }
    public S echo(S value) {
        ++calls;
        if (failure != null) throw failure;
        last = value;
        return last;
    }
    public Object hidden(Object value) { ++ownCalls; return value; }
    public int mode(int position) { return seed + position + 2; }
}
