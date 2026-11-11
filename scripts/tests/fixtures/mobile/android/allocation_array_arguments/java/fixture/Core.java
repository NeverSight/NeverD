package fixture;
public final class Core {
    public static Target literal(Holder holder, int size, Object[] outside, int index, int tag) {
        String[] array;
        Target result = new Target(array = new String[]{Trace.read(holder)}, new int[]{tag});
        Trace.alias = array;
        Trace.step(8);
        return result;
    }
    public static Target sized(Holder holder, int size, Object[] outside, int index, int tag) {
        String[] array;
        Target result = new Target(array = new String[size], new int[]{Trace.number(tag)});
        Trace.alias = array;
        Trace.step(8);
        return result;
    }
    public static Target mutate(Holder holder, int size, Object[] outside, int index, int tag) {
        String[] array;
        Target result = new Target(array = new String[]{(String) (outside[index] = Trace.read(holder))}, new int[]{tag});
        Trace.alias = array;
        Trace.step(8);
        return result;
    }
}
