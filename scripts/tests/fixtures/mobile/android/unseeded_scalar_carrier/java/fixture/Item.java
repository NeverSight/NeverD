package fixture;
public class Item implements Carrier<Object> {
    public int calls;
    public Object last;
    public boolean accepts(Object input) {
        Trace.hit(2);
        calls++;
        last = input;
        if (input != null) ((int[]) input)[0]++;
        Trace.hit(3);
        return input == Trace.token;
    }
}
