package fixture;
public class Core implements Api<String> {
    public String selected;
    public RuntimeException failure;
    public Object tag;
    public int calls;
    @Stamp(value=11, label="owned")
    public String get(Object tag) {
        calls++;
        this.tag = tag;
        if (failure != null) throw failure;
        return selected;
    }
}
