package fixture;
public class Core {
    public static Target make(Holder holder) {
        Target result = new Target(holder.link.value, holder.count);
        Trace.after = result.value;
        Trace.afterInt = result.number;
        Trace.hit(6);
        return result;
    }
    public static Target live(Holder holder) {
        Object saved;
        int number;
        Target result = new Target(saved = holder.link.value, number = holder.count);
        Trace.after = saved;
        Trace.afterInt = number;
        Trace.hit(6);
        return result;
    }
    public static Target keepLink(Holder holder) {
        Link saved;
        Target result = new Target((saved = holder.link).value, holder.count);
        Trace.after = result.value;
        Trace.afterInt = result.number;
        Trace.afterLink = saved;
        Trace.hit(6);
        return result;
    }
}
