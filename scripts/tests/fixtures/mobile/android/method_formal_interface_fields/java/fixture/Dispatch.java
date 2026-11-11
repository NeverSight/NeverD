package fixture;

public class Dispatch {
    public static Object plain(FormalView view, Object key) {
        Receiver receiver = (Receiver) view;
        receiver.calls++;
        receiver.last = key;
        if (receiver.failure != null) throw receiver.failure;
        return key;
    }
}
