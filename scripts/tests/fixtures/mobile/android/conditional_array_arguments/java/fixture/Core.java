package fixture;
import java.io.IOException;

public final class Core {
    public static Result make(boolean flag, int size, Object left, Object right) throws IOException {
        Object[] values;
        return new Result(
            flag ? Trace.probe.select(left) : Trace.probe.select(right),
            Trace.probe.finish(
                values = Trace.capture(new String[size]),
                values[0] = Trace.probe.element(0, left),
                values[1] = Trace.probe.element(1, right)));
    }
}
