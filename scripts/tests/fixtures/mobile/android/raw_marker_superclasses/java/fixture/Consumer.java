package fixture;

public class Consumer extends Root<String> implements Factory<Integer,Long,String> {
    public String result;
    public int calls;
    public RuntimeException failure;
    public Consumer() {}

    public String get() {
        calls++;
        if (failure != null) throw failure;
        return result;
    }
}
