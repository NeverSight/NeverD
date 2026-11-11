package fixture;

public final class Cursor implements java.util.Iterator<Object> {
    private final Object token;
    private boolean used;
    public Cursor(Object token) { this.token = token; }
    public boolean hasNext() { return !used; }
    public Object next() {
        if (used) throw new java.util.NoSuchElementException();
        used = true;
        return token;
    }
    public void remove() { throw new UnsupportedOperationException(); }
}
