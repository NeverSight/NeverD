package fixture;
public final class Client {
    public static Object call(Policy policy,Object input,Tag tag,Callback callback,Completion completion) { return policy.work(input,tag,callback,completion); }
}
