package fixture;
public class Core extends Middle<Payload> {
    public static int events;
    public int stage;
    public void submit(Payload value) {
        events = events * 5 + 3;
        stage = 1;
        super.deliver(value);
        stage = 2;
        events = events * 5 + 4;
    }
    public void checked(Object value) {
        events = events * 5 + 3;
        stage = 1;
        Payload argument = (Payload) value;
        super.deliver(argument);
        stage = 2;
        events = events * 5 + 4;
    }
}
