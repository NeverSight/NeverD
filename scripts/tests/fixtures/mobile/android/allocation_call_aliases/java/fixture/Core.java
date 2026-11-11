package fixture;
public final class Core {
    public static Target make(Holder holder, boolean flag) {
        Target result = new Target(holder.first, Provider.read(holder.second), flag);
        Trace.after = result.second;
        return result;
    }
    public static Target live(Holder holder, boolean flag) {
        Object saved;
        Target result = new Target(holder.first, saved = Provider.read(holder.second), flag);
        Trace.after = saved;
        return result;
    }
}
