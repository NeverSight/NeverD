package fixture;
public final class Trace {
    public static long events;
    public static long input;
    public static int failAt;
    public static Owner partial;
    public static Hook hook;
    public static RuntimeException failure;
    public static boolean[] flags;
    public static long[] values;
    public static long[] bases;
    public static int[] children;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
    public static void observe(Owner owner,int stage) {
        partial=owner;flags[stage]=owner.zeroFlag;values[stage]=owner.value;
        bases[stage]=owner.baseValue;children[stage]=((Child)owner).childValue;
        if(hook!=null)hook.observe(owner,stage);
        hit(stage);
    }
}
