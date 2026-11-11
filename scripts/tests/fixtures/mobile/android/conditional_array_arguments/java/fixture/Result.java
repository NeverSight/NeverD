package fixture;
import java.io.IOException;

public final class Result {
    static { Trace.probe.initialize(); }
    public final Object selected;
    public final Object[] values;

    public Result(Object selected, Object[] values) throws IOException {
        this.selected = selected;
        this.values = values;
        Trace.probe.constructed(this);
    }
}
