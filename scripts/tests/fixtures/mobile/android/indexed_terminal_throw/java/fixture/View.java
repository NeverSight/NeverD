package fixture;
@SuppressWarnings("rawtypes")
public final class View<S> implements Contract {
    public View() {}
    public boolean add(Object value) {
        throw new UnsupportedOperationException("owned immutable operation");
    }
    public boolean add(Element value) {
        throw new UnsupportedOperationException("owned immutable operation");
    }
    public void insert(int index, Object value) {
        throw new UnsupportedOperationException("owned immutable operation");
    }
    public void insert(int index, Element value) {
        throw new UnsupportedOperationException("owned immutable operation");
    }
    public Object replace(int index, Object value) {
        throw new UnsupportedOperationException("owned immutable operation");
    }
    public Element replace(int index, Element value) {
        throw new UnsupportedOperationException("owned immutable operation");
    }
    public Element remove(int index) {
        throw new UnsupportedOperationException("owned immutable operation");
    }
    public void begin(Object value) {
        throw new UnsupportedOperationException("owned immutable operation");
    }
    public void begin(Element value) {
        throw new UnsupportedOperationException("owned immutable operation");
    }
    public Element first() {
        throw new UnsupportedOperationException("owned immutable operation");
    }
}
