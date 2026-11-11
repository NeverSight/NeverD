package fixture;
public final class Dispatch {
    public static int compare(Api receiver, Api argument) {
        int first = receiver.observe(argument);
        int second = argument.observe(receiver);
        return first - second;
    }
}
