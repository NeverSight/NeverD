package fixture;
class Provider {
    static { Trace.initializations++; }
    public static final <T> Object[] adapt(T[] values, boolean reuse) {
        Trace.hit(1);
        Trace.input = values;
        Trace.flag = reuse;
        Trace.calls++;
        Trace.action.run();
        Trace.hit(2);
        Object[] chosen;
        if (Trace.override) chosen = Trace.result;
        else if (reuse && values.getClass() == Object[].class) chosen = values;
        else chosen = java.util.Arrays.copyOf(values, values.length, Object[].class);
        Trace.chosen = chosen;
        Trace.hit(3);
        return chosen;
    }
}
