package fixture;
public class Root {
    public int stamp;
    public int touches;
    public Object seen;
    public RuntimeException failure;
    public Root() { stamp = 7; }
    protected void touch(Object marker) {
        touches++;
        seen = marker;
        if (failure != null) throw failure;
    }
}
