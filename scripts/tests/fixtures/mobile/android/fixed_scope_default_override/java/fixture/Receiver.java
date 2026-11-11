package fixture;
public class Receiver extends Middle<String> implements Api<String> {
    public int calls;
    public int overloads;
    public String last;
    public RuntimeException failure;
    public Receiver() { }
    @Override public String echo(String value) {
        ++calls;
        last = value;
        if (failure != null) throw failure;
        return value;
    }
    public String echo(Integer value) { ++overloads; return "overload"; }
}
