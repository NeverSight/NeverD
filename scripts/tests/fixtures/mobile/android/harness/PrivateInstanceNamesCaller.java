import fixture.Provider;
public class PrivateInstanceNamesCaller {
    public static long call(Provider<?> receiver, long first, long second) { return receiver.call(first,second); }
}
