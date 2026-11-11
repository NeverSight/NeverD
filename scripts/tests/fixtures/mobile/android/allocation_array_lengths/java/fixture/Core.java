package fixture;
public final class Core {
    public static Target direct(Holder holder, int[] input, boolean flag) {
        Target result = new Target(input.length, null, flag);
        Trace.after = result.length; Trace.hit(3);
        return result;
    }
    public static Target field(Holder holder, int[] input, boolean flag) {
        Target result = new Target(holder.array.length, null, flag);
        Trace.after = result.length; Trace.hit(3);
        return result;
    }
    public static Target live(Holder holder, int[] input, boolean flag) {
        int saved;
        Target result = new Target(saved = holder.array.length, null, flag);
        Trace.after = saved; Trace.hit(3);
        return result;
    }
    public static Target nested(Holder holder, int[] input, boolean flag) {
        Target result = new Target(holder.array.length, new Inner(), flag);
        Trace.after = result.length; Trace.hit(3);
        return result;
    }
}
