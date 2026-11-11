package fixture;
public final class Outcome implements java.io.Serializable {
    private final String name;
    private final Object[] captured;
    public Outcome(String name,Object[] captured) { this.name=name;this.captured=captured; }
    public String getImplMethodName() { return name; }
    public String getFunctionalInterfaceMethodName() { return "apply"; }
    public int getCapturedArgCount() { return captured.length; }
    public Object getCapturedArg(int index) { return captured[index]; }
}
