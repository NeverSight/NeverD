package fixture;
public final class Client {
    public static Object call(Policy policy,Callback callback,Completion completion) { return policy.work(callback,completion); }
}
