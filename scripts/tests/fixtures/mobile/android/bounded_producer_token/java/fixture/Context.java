package fixture;
public class Context {
    public Result reply;public boolean eager;
    public Context() {}
    public interface Factory { Object marker(); }
}
