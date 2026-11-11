package fixture;
public class Context {
    public static final Object SUSPENDED = new Object();
    public static Context installed;
    public static Callback block;
    public Scope scope;
    public InterruptedException interruption;
}
