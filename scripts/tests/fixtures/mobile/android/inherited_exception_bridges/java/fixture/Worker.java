package fixture;

public final class Worker implements java.util.concurrent.Callable<String>, Echo<String> {
    public String value;
    public String last;
    public int calls;
    public int echoes;
    public RuntimeException failure;

    public String call() {
        calls++;
        if (failure != null) throw failure;
        return value;
    }

    public String echo(String item) {
        echoes++;
        last = item;
        if (failure != null) throw failure;
        return item;
    }
}
