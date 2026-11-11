package fixture;
public class Client {
    public static Result call(Api receiver, Object input, Box callback, long value) {
        return receiver.read(input, callback, value);
    }
}
