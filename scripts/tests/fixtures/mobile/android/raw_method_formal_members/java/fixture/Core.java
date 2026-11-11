package fixture;
import java.util.ArrayList;
import java.util.List;

public final class Core implements Api {
    public int calls;
    public int marker;
    public Object observed;
    public Object lastInput;
    public RuntimeException failure;
    public List<Object> seen = new ArrayList<Object>();
    @Override public Object[] array(Object[] input, Object tag) {
        return observe(input, tag);
    }
    public Object[] entry(Object[] input, Object tag) {
        return array(input, tag);
    }
    @Override public Object[] observe(Object[] input, Object tag) {
        calls++;
        marker = marker * 3 + 1;
        observed = tag;
        lastInput = input;
        seen.add(tag);
        if (tag instanceof int[]) ((int[]) tag)[0] += 5;
        if (failure != null) throw failure;
        return input;
    }
}
