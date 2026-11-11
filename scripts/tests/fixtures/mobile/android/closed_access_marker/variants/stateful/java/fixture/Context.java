package fixture;
public class Context {
    public RuntimeException failure;
    public Object token;
    public static int initializations;
    public static Runnable initializer;
    public static void initialize() { initializations++; initializer.run(); }
}
