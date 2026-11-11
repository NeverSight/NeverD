package fixture;

public class Parent<P extends java.io.Serializable> extends Root<P> {
    public P value;
    public int parentCalls;
    public Parent() {}
    public P parse(String text) { parentCalls++; return value; }
}
