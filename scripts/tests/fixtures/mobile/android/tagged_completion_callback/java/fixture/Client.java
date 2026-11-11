package fixture;
public final class Client {
    public static Object call(Policy policy,Tag tag,Callback callback,Completion completion) { return policy.work(tag,callback,completion); }
}
